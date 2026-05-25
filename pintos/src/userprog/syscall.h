#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
void syscall_filesys_lock_acquire (void);
void syscall_filesys_lock_release (void);
void syscall_do_munmap_all (void);

#endif /* userprog/syscall.h */
