#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>

struct thread;
struct sup_page_entry;

struct frame
  {
    void *kernel_vaddr;
    void *user_vaddr;

    struct thread *owner;

    struct list_elem elem;

    struct sup_page_entry *aux;
  };

void frame_table_init (void);

void *frame_alloc (void *user_vaddr);
void frame_free (void *kernel_vaddr);

#endif /* vm/frame.h */
