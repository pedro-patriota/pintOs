#include "vm/frame.h"
#include <debug.h>
#include <hash.h>
#include <list.h>
#include <stdlib.h>
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

struct frame_table_entry
  {
    void *kernel_vaddr;
    void *user_vaddr;

    struct thread *owner;

    struct list_elem list_elem;
    struct hash_elem hash_elem;
  };

static struct lock frame_table_lock;
static struct list frame_table_list;
static struct hash frame_table_map;

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

  lock_acquire (&frame_table_lock);

  kernel_vaddr = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kernel_vaddr == NULL)
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

  hash_insert (&frame_table_map, &fte->hash_elem);
  list_push_back (&frame_table_list, &fte->list_elem);

RELEASE:
  lock_release (&frame_table_lock);
  return kernel_vaddr;
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
