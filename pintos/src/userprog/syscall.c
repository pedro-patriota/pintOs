#include "userprog/syscall.h"
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <syscall-nr.h>
#include "userprog/pagedir.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"

static void
syscall_handler (struct intr_frame *);

/* Read a 32-bit value from user virtual address UADDR.
   Returns the value, or terminates the process if UADDR
   is invalid. */
static void
validate_user_address (const uint8_t *uaddr)
{
  struct thread *cur = thread_current ();

  if (uaddr == NULL
      || !is_user_vaddr (uaddr)
      || cur->pagedir == NULL
      || pagedir_get_page (cur->pagedir, uaddr) == NULL)
    thread_exit_verbose (-1);
}

static uint8_t
get_user_byte (const uint8_t *uaddr)
{
  validate_user_address (uaddr);
  return *(uint8_t *) pagedir_get_page (thread_current ()->pagedir, uaddr);
}

static uint32_t
get_user_word (const void *uaddr)
{
  const uint8_t *bytes = uaddr;
  uint32_t word = 0;
  size_t i;

  for (i = 0; i < sizeof (uint32_t); i++)
    word |= (uint32_t) get_user_byte (bytes + i) << (i * 8);
  return word;
}

static void
copy_in (void *dst_, const void *usrc_, size_t size)
{
  uint8_t *dst = dst_;
  const uint8_t *usrc = usrc_;
  size_t i;

  for (i = 0; i < size; i++)
    dst[i] = get_user_byte (usrc + i);
}

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f)
{
  uint32_t *esp = (uint32_t *) f->esp;
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
        int fd = (int) get_user_word (esp + 1);
        const uint8_t *buffer = (const uint8_t *) get_user_word (esp + 2);
        unsigned size = (unsigned) get_user_word (esp + 3);

        if (fd == 1)
          {
            unsigned written = 0;

            while (written < size)
              {
                uint8_t chunk[128];
                size_t chunk_size = size - written;
                if (chunk_size > sizeof chunk)
                  chunk_size = sizeof chunk;

                copy_in (chunk, buffer + written, chunk_size);
                putbuf ((char *) chunk, chunk_size);
                written += chunk_size;
              }

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
