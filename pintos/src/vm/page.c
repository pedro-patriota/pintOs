#include "vm/page.h"
#include <debug.h>
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/frame.h"
#ifdef USERPROG
#include "userprog/pagedir.h"
#endif

static bool
install_page (void *user_vaddr, void *kernel_vaddr, bool is_writable)
{
  struct thread *t = thread_current ();

  ASSERT (pg_ofs (user_vaddr) == 0);
  ASSERT (pg_ofs (kernel_vaddr) == 0);

#ifdef USERPROG
  if (pagedir_get_page (t->pagedir, user_vaddr) != NULL)
    return false;
  return pagedir_set_page (t->pagedir, user_vaddr, kernel_vaddr, is_writable);
#else
  return false;
#endif
}

static unsigned
sup_page_table_hash_func (const struct hash_elem *elem, void *aux UNUSED)
{
  struct sup_page_entry *spe =
      hash_entry (elem, struct sup_page_entry, elem);
  return hash_bytes (&spe->user_vaddr, sizeof (spe->user_vaddr));
}

static bool
sup_page_table_less_func (const struct hash_elem *a, const struct hash_elem *b,
                          void *aux UNUSED)
{
  struct sup_page_entry *spe_a =
      hash_entry (a, struct sup_page_entry, elem);
  struct sup_page_entry *spe_b =
      hash_entry (b, struct sup_page_entry, elem);
  return spe_a->user_vaddr < spe_b->user_vaddr;
}

void
sup_page_table_init (struct sup_page_table *spt)
{
  hash_init (&spt->table, sup_page_table_hash_func, sup_page_table_less_func,
             NULL);
}

bool
sup_page_table_insert (struct sup_page_table *spt, struct sup_page_entry *spe)
{
  struct hash_elem *e = hash_find (&spt->table, &spe->elem);

  if (e != NULL)
    return false;

  hash_insert (&spt->table, &spe->elem);

  return true;
}

void
sup_page_table_remove (struct sup_page_table *spt, struct sup_page_entry *spe)
{
  hash_delete (&spt->table, &spe->elem);
  free (spe);
}

struct sup_page_entry *
sup_page_table_lookup (struct sup_page_table *table, void *user_vaddr)
{
  struct sup_page_entry key = {0};
  struct hash_elem *e = NULL;

  key.user_vaddr = pg_round_down (user_vaddr);
  e = hash_find (&table->table, &key.elem);

  if (e == NULL)
    return NULL;

  return hash_entry (e, struct sup_page_entry, elem);
}

bool
load_page (struct sup_page_entry *spe)
{
  void *kernel_vaddr = NULL;
  size_t file_size = 0;
  bool ok = true;

  kernel_vaddr = frame_alloc (spe->user_vaddr);
  if (kernel_vaddr == NULL)
    {
      ok = false;
      goto CLEANUP;
    }
  if (spe->flags & SUP_PAGE_SWAPPED)
    {
      swap_in ((int) spe->swap_slot, kernel_vaddr);
      spe->flags &= ~SUP_PAGE_SWAPPED;
    }
  else if (spe->file != NULL)
    {
      file_seek (spe->file, spe->offset);

      file_size = (size_t)file_read (spe->file, kernel_vaddr, spe->read_bytes);

      if (file_size != spe->read_bytes)
        {
          ok = false;
          goto CLEANUP;
        }

      memset ((uint8_t *) kernel_vaddr + spe->read_bytes, 0, spe->zero_bytes);
    }
  else
    memset(kernel_vaddr, 0, PGSIZE);

  if (!install_page (spe->user_vaddr, kernel_vaddr,
                     spe->flags & SUP_PAGE_WRITABLE))
    ok = false;

CLEANUP:
  if (!ok && kernel_vaddr != NULL)
    frame_free (kernel_vaddr);

  return ok;
}
