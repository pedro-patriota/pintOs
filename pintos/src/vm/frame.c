#include "vm/frame.h"
#include <debug.h>
#include <hash.h>
#include <list.h>
#include <stdlib.h>
#include "filesys/file.h"
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
  swap_init ();
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
    {
      /* Evict a frame. */
      struct list_elem *e;
      for (e = list_begin (&frame_table_list); e != list_end (&frame_table_list);
           e = list_next (e))
        {
          struct frame_table_entry *victim = list_entry (e, struct frame_table_entry, list_elem);
          struct thread *owner = victim->owner;
          struct sup_page_entry *spe = NULL;

          if (owner != NULL)
            spe = sup_page_table_lookup (&owner->spt, victim->user_vaddr);

          bool dirty = false;
          if (owner != NULL && owner->pagedir != NULL)
            dirty = pagedir_is_dirty (owner->pagedir, victim->user_vaddr);

          /* If the page was recently accessed, give it a second chance. */
          if (owner != NULL && owner->pagedir != NULL &&
              pagedir_is_accessed (owner->pagedir, victim->user_vaddr))
            {
              printf ("EVICT: second-chance victim user=%p owner=%s\n",
                      victim->user_vaddr,
                      owner ? owner->name : "(null)");
              /* Clear accessed and move to back of list. */
              pagedir_set_accessed (owner->pagedir, victim->user_vaddr, false);
              list_remove (&victim->list_elem);
              list_push_back (&frame_table_list, &victim->list_elem);
              continue;
            }

          if (spe != NULL && spe->file != NULL)
            {
              if (dirty)
                {
                  printf ("EVICT: writing back file-backed page user=%p kernel=%p owner=%s read_bytes=%u offset=%llu\n",
                          victim->user_vaddr, victim->kernel_vaddr,
                          owner ? owner->name : "(null)", (unsigned) spe->read_bytes,
                          (unsigned long long) spe->offset);
                  syscall_filesys_lock_acquire ();
                  file_write_at (spe->file, victim->kernel_vaddr, spe->read_bytes, spe->offset);
                  syscall_filesys_lock_release ();
                }
            }
          else if (spe != NULL)
            {
              if (dirty)
                {
                  printf ("EVICT: swapping out anon page user=%p kernel=%p owner=%s\n",
                          victim->user_vaddr, victim->kernel_vaddr,
                          owner ? owner->name : "(null)");
                  int slot = swap_out (victim->kernel_vaddr);
                  if (slot < 0)
                    {
                      printf ("EVICT: swap_out failed for kernel=%p\n", victim->kernel_vaddr);
                      continue;
                    }
                  spe->swap_slot = (block_sector_t) slot;
                  spe->flags |= SUP_PAGE_SWAPPED;
                  printf ("EVICT: swapped to slot %d\n", slot);
                }
            }

          if (owner != NULL && owner->pagedir != NULL)
            {
              printf ("EVICT: clearing pagedir for user=%p owner=%s\n",
                      victim->user_vaddr, owner ? owner->name : "(null)");
              pagedir_clear_page (owner->pagedir, victim->user_vaddr);
            }

          /* Reuse this victim's physical page. */
          kernel_vaddr = victim->kernel_vaddr;
          victim->user_vaddr = user_vaddr;
          victim->owner = thread_current ();
            printf ("EVICT: reusing kernel=%p for new user=%p owner=%s\n",
              kernel_vaddr, user_vaddr, thread_current ()->name);
            list_remove (&victim->list_elem);
            list_push_back (&frame_table_list, &victim->list_elem);
          break;
        }
    }

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
