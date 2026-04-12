#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"

static void
syscall_handler (struct intr_frame *);

/* Read a 32-bit value from user virtual address UADDR.
   Returns the value, or terminates the process if UADDR
   is invalid. */
static uint32_t
get_user_word (const uint32_t *uaddr)
{
  if (!is_user_vaddr (uaddr))
    thread_exit_verbose (-1);
  return *uaddr;
}

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f)
{
  uint32_t *esp = (uint32_t *)f->esp;
  int syscall_num = get_user_word (esp);

  switch (syscall_num)
    {
    case SYS_HALT:
      shutdown_power_off ();
      NOT_REACHED ();

    case SYS_EXIT:
      {
        int status = (int)get_user_word (esp + 1);
        f->eax = status;
        thread_exit_verbose (status);
        NOT_REACHED ();
      }

    case SYS_WRITE:
      {
        int fd = (int)get_user_word (esp + 1);
        const char *buff = (const char *)get_user_word (esp + 2);
        /* Since get_user_word returns a 32-bit value, we cast it to
           unsigned int instead of size_t. */
        unsigned int size = (unsigned int)get_user_word (esp + 3);

        if (fd == 1)
          {
            putbuf (buff, size);
            f->eax = size;
          }
        else
          f->eax = -1;

        break;
      }

    default:
      thread_exit_verbose (-1);
      break;
    }
}
