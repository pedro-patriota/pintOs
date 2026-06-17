#include "vm/frame.h"
#include <debug.h>
#include <hash.h>
#include <list.h>
#include <stdlib.h>
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/swap.h"

struct frame_table_entry
  {
    void *kernel_vaddr;
    void *user_vaddr;

    struct thread *owner;
    bool pinned;

    struct list_elem list_elem;
    struct hash_elem hash_elem;
  };

static struct lock frame_table_lock;
static struct list frame_table_list;
static struct hash frame_table_map;

static void *evict_frame (void *new_user_vaddr);
static bool evict_page (struct frame_table_entry *);

static unsigned
frame_table_map_hash_func (const struct hash_elem *elem, void *aux UNUSED)
{
  struct frame_table_entry *fte =
      hash_entry (elem, struct frame_table_entry, hash_elem);
  return hash_bytes (&fte->kernel_vaddr, sizeof (fte->kernel_vaddr));
}

static bool
frame_table_map_less_func (const struct hash_elem *a, const struct hash_elem *b,
                           void *aux UNUSED)
{
  struct frame_table_entry *fte_a =
      hash_entry (a, struct frame_table_entry, hash_elem);
  struct frame_table_entry *fte_b =
      hash_entry (b, struct frame_table_entry, hash_elem);
  return fte_a->kernel_vaddr < fte_b->kernel_vaddr;
}

void
frame_table_init (void)
{
  lock_init (&frame_table_lock);
  list_init (&frame_table_list);
  hash_init (&frame_table_map, frame_table_map_hash_func,
             frame_table_map_less_func, NULL);
}

void *
frame_alloc (void *user_vaddr)
{
  ASSERT (is_user_vaddr (user_vaddr));
  ASSERT (pg_ofs (user_vaddr) == 0);

  struct frame_table_entry *fte = NULL;
  void *kernel_vaddr = NULL;
  bool reused_frame = false;

  lock_acquire (&frame_table_lock);

  kernel_vaddr = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kernel_vaddr == NULL)
    {
      kernel_vaddr = evict_frame (user_vaddr);
      reused_frame = kernel_vaddr != NULL;
    }

  if (kernel_vaddr == NULL || reused_frame)
    goto RELEASE;

  fte = calloc (1, sizeof (struct frame_table_entry));
  if (fte == NULL)
    {
      palloc_free_page (kernel_vaddr);
      kernel_vaddr = NULL;
      goto RELEASE;
    }
  fte->kernel_vaddr = kernel_vaddr;
  fte->user_vaddr = user_vaddr;
  fte->owner = thread_current ();
  fte->pinned = true;

  hash_insert (&frame_table_map, &fte->hash_elem);
  list_push_back (&frame_table_list, &fte->list_elem);

RELEASE:
  lock_release (&frame_table_lock);
  return kernel_vaddr;
}

static void *
evict_frame (void *new_user_vaddr)
{
  size_t scans;
  size_t limit;

  if (list_empty (&frame_table_list))
    return NULL;

  limit = list_size (&frame_table_list) * 2;
  for (scans = 0; scans < limit; scans++)
    {
      struct list_elem *e = list_pop_front (&frame_table_list);
      struct frame_table_entry *victim =
        list_entry (e, struct frame_table_entry, list_elem);
      struct thread *owner = victim->owner;

      if (owner == NULL || owner->pagedir == NULL)
        {
          list_push_back (&frame_table_list, &victim->list_elem);
          continue;
        }

      if (victim->pinned)
        {
          list_push_back (&frame_table_list, &victim->list_elem);
          continue;
        }

      if (pagedir_is_accessed (owner->pagedir, victim->user_vaddr))
        {
          pagedir_set_accessed (owner->pagedir, victim->user_vaddr, false);
          list_push_back (&frame_table_list, &victim->list_elem);
          continue;
        }

      if (!evict_page (victim))
        {
          list_push_back (&frame_table_list, &victim->list_elem);
          continue;
        }

      victim->user_vaddr = new_user_vaddr;
      victim->owner = thread_current ();
      victim->pinned = true;
      list_push_back (&frame_table_list, &victim->list_elem);
      return victim->kernel_vaddr;
    }

  return NULL;
}

static bool
evict_page (struct frame_table_entry *victim)
{
  struct thread *owner = victim->owner;
  struct sup_page_entry *spe;
  bool dirty;

  ASSERT (owner != NULL);
  ASSERT (owner->pagedir != NULL);

  spe = sup_page_table_lookup (&owner->spt, victim->user_vaddr);
  if (spe == NULL)
    return false;

  dirty = pagedir_is_dirty (owner->pagedir, victim->user_vaddr);
  pagedir_clear_page (owner->pagedir, victim->user_vaddr);

  if (spe->type == SUP_PAGE_MMAP)
    {
      if (dirty)
        {
          syscall_filesys_lock_acquire ();
          file_write_at (spe->file, victim->kernel_vaddr, spe->read_bytes,
                         spe->offset);
          syscall_filesys_lock_release ();
        }
    }
  else if (spe->type == SUP_PAGE_ANON || dirty)
    {
      int slot = swap_out (victim->kernel_vaddr);
      if (slot < 0)
        PANIC ("Swap partition is full");

      spe->swap_slot = (block_sector_t) slot;
      spe->flags |= SUP_PAGE_SWAPPED;
      spe->type = SUP_PAGE_ANON;
    }

  return true;
}

void
frame_unpin (void *kernel_vaddr)
{
  struct frame_table_entry fte_key = {0};
  struct hash_elem *hash_elem = NULL;

  ASSERT (is_kernel_vaddr (kernel_vaddr));
  ASSERT (pg_ofs (kernel_vaddr) == 0);

  lock_acquire (&frame_table_lock);

  fte_key.kernel_vaddr = kernel_vaddr;
  hash_elem = hash_find (&frame_table_map, &fte_key.hash_elem);
  if (hash_elem != NULL)
    {
      struct frame_table_entry *fte =
        hash_entry (hash_elem, struct frame_table_entry, hash_elem);
      fte->pinned = false;
    }

  lock_release (&frame_table_lock);
}

void
frame_free (void *kernel_vaddr)
{
  ASSERT (is_kernel_vaddr (kernel_vaddr));
  ASSERT (pg_ofs (kernel_vaddr) == 0);

  struct frame_table_entry *fte = NULL;
  struct frame_table_entry fte_key = {0};
  struct hash_elem *hash_elem = NULL;

  lock_acquire (&frame_table_lock);

  fte_key.kernel_vaddr = kernel_vaddr;

  hash_elem = hash_find (&frame_table_map, &fte_key.hash_elem);
  if (hash_elem == NULL)
    goto RELEASE;

  fte = hash_entry (hash_elem, struct frame_table_entry, hash_elem);

  hash_delete (&frame_table_map, &fte->hash_elem);
  list_remove (&fte->list_elem);

  palloc_free_page (kernel_vaddr);
  free (fte);

RELEASE:
  lock_release (&frame_table_lock);
}
