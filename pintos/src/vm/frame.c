#include "vm/frame.h"
#include <debug.h>
#include <stdlib.h>
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "vm/page.h"

static struct lock frame_lock;
static struct list frame_list;

void
frame_table_init (void)
{
  list_init (&frame_list);
  lock_init (&frame_lock);
}

void *
frame_alloc (void *user_vaddr)
{
  struct frame *f = NULL;
  void *kernel_vaddr = NULL;

  lock_acquire (&frame_lock);

  kernel_vaddr = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kernel_vaddr == NULL)
    goto RELEASE;

  f = malloc (sizeof (struct frame));
  f->kernel_vaddr = kernel_vaddr;
  f->user_vaddr = user_vaddr;
  f->owner = thread_current ();

  list_push_back (&frame_list, &f->elem);

RELEASE:
  lock_release (&frame_lock);
  return kernel_vaddr;
}

void
frame_free (void *kernel_vaddr)
{
}
