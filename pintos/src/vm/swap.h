#ifndef VM_SWAP_H
#define VM_SWAP_H

void swap_init (void);
void swap_in (int slot, void *kernel_vaddr);
int swap_out (void *kernel_vaddr);
void swap_free (int slot);

#endif /* vm/swap.h */
