#ifndef VM_FRAME_H
#define VM_FRAME_H

void frame_table_init (void);

void *frame_alloc (void *user_vaddr);
void frame_free (void *kernel_vaddr);

#endif /* vm/frame.h */
