#include "userprog/syscall.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "vm/frame.h"
#include "../lib/user/syscall.h"

static struct lock filesys_lock;

struct mmap_region
  {
    int mapid;
    void *addr;
    size_t length;
    struct file *file;
    struct list_elem elem;
  };

static void syscall_handler (struct intr_frame *);
static void validate_user_address (const uint8_t *);
static void validate_user_buffer (const void *, unsigned);
static void validate_user_writable_address (const uint8_t *);
static void validate_user_writable_buffer (void *, unsigned);
static uint8_t get_user_byte (const uint8_t *);
static uint32_t get_user_word (const void *);
static void copy_in (void *, const void *, size_t);
static void copy_out (void *, const void *, size_t);
static char *copy_in_string (const char *);
static struct file *get_file_from_fd (int);
static int allocate_fd (struct file *);
static void close_fd (int);

void
syscall_filesys_lock_acquire (void)
{
  lock_acquire (&filesys_lock);
}

void
syscall_do_munmap_all (void)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  for (e = list_begin (&cur->mmap_list); e != list_end (&cur->mmap_list); )
    {
      struct mmap_region *m = list_entry (e, struct mmap_region, elem);
      e = list_next (e);

      size_t page_cnt = (m->length + PGSIZE - 1) / PGSIZE;
      size_t i;
      for (i = 0; i < page_cnt; i++)
        {
          void *uaddr = m->addr + i * PGSIZE;
          struct sup_page_entry *spe = sup_page_table_lookup (&cur->spt, uaddr);
          if (spe == NULL)
            continue;

          void *kpage = pagedir_get_page (cur->pagedir, uaddr);
          if (kpage != NULL)
            {
              if (pagedir_is_dirty (cur->pagedir, uaddr) && spe->file != NULL)
                {
                  syscall_filesys_lock_acquire ();
                  file_write_at (spe->file, kpage, spe->read_bytes, spe->offset);
                  syscall_filesys_lock_release ();
                }
              pagedir_clear_page (cur->pagedir, uaddr);
              frame_free (pg_round_down (kpage));
            }
          else if (spe->flags & SUP_PAGE_SWAPPED)
            {
              swap_free ((int) spe->swap_slot);
            }

          sup_page_table_remove (&cur->spt, spe);
        }

      syscall_filesys_lock_acquire ();
      file_allow_write (m->file);
      file_close (m->file);
      syscall_filesys_lock_release ();

      list_remove (&m->elem);
      free (m);
    }
}

void
syscall_filesys_lock_release (void)
{
  lock_release (&filesys_lock);
}

void
syscall_init (void)
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
validate_user_address (const uint8_t *uaddr)
{
  struct thread *cur = thread_current ();

  if (uaddr == NULL
      || !is_user_vaddr (uaddr)
      || cur->pagedir == NULL
      || pagedir_get_page (cur->pagedir, uaddr) == NULL)
    process_exit_with_status (-1);
}

static void
validate_user_buffer (const void *uaddr_, unsigned size)
{
  const uint8_t *uaddr = uaddr_;
  unsigned i;

  if (size == 0)
    return;

  for (i = 0; i < size; i++)
    validate_user_address (uaddr + i);
}

static void
validate_user_writable_address (const uint8_t *uaddr)
{
  validate_user_address (uaddr);
  if (!pagedir_is_writable (thread_current ()->pagedir, uaddr))
    process_exit_with_status (-1);
}

static void
validate_user_writable_buffer (void *uaddr_, unsigned size)
{
  uint8_t *uaddr = uaddr_;
  unsigned i;

  if (size == 0)
    return;

  for (i = 0; i < size; i++)
    validate_user_writable_address (uaddr + i);
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

  for (i = 0; i < sizeof word; i++)
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

static void
copy_out (void *udst_, const void *src_, size_t size)
{
  uint8_t *udst = udst_;
  const uint8_t *src = src_;
  size_t i;

  for (i = 0; i < size; i++)
    {
      validate_user_writable_address (udst + i);
      *(uint8_t *) pagedir_get_page (thread_current ()->pagedir, udst + i)
        = src[i];
    }
}

static char *
copy_in_string (const char *ustr)
{
  char *kstr = palloc_get_page (0);
  size_t i;

  if (kstr == NULL)
    return NULL;

  for (i = 0; i < PGSIZE; i++)
    {
      kstr[i] = get_user_byte ((const uint8_t *) ustr + i);
      if (kstr[i] == '\0')
        return kstr;
    }

  palloc_free_page (kstr);
  process_exit_with_status (-1);
}

static struct file *
get_file_from_fd (int fd)
{
  if (fd < 2 || fd >= MAX_FD)
    return NULL;
  return thread_current ()->fd_table[fd];
}

static int
allocate_fd (struct file *file)
{
  struct thread *cur = thread_current ();
  int i;

  for (i = 2; i < MAX_FD; i++)
    {
      int fd = cur->next_fd;
      if (fd < 2 || fd >= MAX_FD)
        fd = 2;

      if (cur->fd_table[fd] == NULL)
        {
          cur->fd_table[fd] = file;
          cur->next_fd = fd + 1;
          if (cur->next_fd >= MAX_FD)
            cur->next_fd = 2;
          return fd;
        }
      cur->next_fd = fd + 1;
    }
  return -1;
}

static void
close_fd (int fd)
{
  struct file *file = get_file_from_fd (fd);

  if (file == NULL)
    return;

  syscall_filesys_lock_acquire ();
  file_close (file);
  syscall_filesys_lock_release ();
  thread_current ()->fd_table[fd] = NULL;
}

static void
syscall_handler (struct intr_frame *f)
{
  uint32_t *esp = (uint32_t *) f->esp;
  int syscall_num = (int) get_user_word (esp);

  switch (syscall_num)
    {
    case SYS_HALT:
      shutdown_power_off ();
      NOT_REACHED ();

    case SYS_EXIT:
      process_exit_with_status ((int) get_user_word (esp + 1));
      NOT_REACHED ();

    case SYS_EXEC:
      {
        const char *ucmd = (const char *) get_user_word (esp + 1);
        char *cmd = copy_in_string (ucmd);
        tid_t tid = TID_ERROR;

        if (cmd != NULL)
          {
            tid = process_execute (cmd);
            palloc_free_page (cmd);
          }
        f->eax = tid;
        break;
      }

    case SYS_WAIT:
      f->eax = process_wait ((tid_t) get_user_word (esp + 1));
      break;

    case SYS_CREATE:
      {
        const char *ufile = (const char *) get_user_word (esp + 1);
        unsigned initial_size = (unsigned) get_user_word (esp + 2);
        char *file = copy_in_string (ufile);
        bool ok = false;

        if (file != NULL)
          {
            syscall_filesys_lock_acquire ();
            ok = filesys_create (file, initial_size);
            syscall_filesys_lock_release ();
            palloc_free_page (file);
          }
        f->eax = ok;
        break;
      }

    case SYS_REMOVE:
      {
        const char *ufile = (const char *) get_user_word (esp + 1);
        char *file = copy_in_string (ufile);
        bool ok = false;

        if (file != NULL)
          {
            syscall_filesys_lock_acquire ();
            ok = filesys_remove (file);
            syscall_filesys_lock_release ();
            palloc_free_page (file);
          }
        f->eax = ok;
        break;
      }

    case SYS_OPEN:
      {
        const char *ufile = (const char *) get_user_word (esp + 1);
        char *file_name = copy_in_string (ufile);
        int fd = -1;

        if (file_name != NULL)
          {
            struct file *file;

            syscall_filesys_lock_acquire ();
            file = filesys_open (file_name);
            if (file != NULL)
              {
                fd = allocate_fd (file);
                if (fd == -1)
                  file_close (file);
              }
            syscall_filesys_lock_release ();
            palloc_free_page (file_name);
          }
        f->eax = fd;
        break;
      }

    case SYS_FILESIZE:
      {
        int fd = (int) get_user_word (esp + 1);
        struct file *file = get_file_from_fd (fd);
        int size = -1;

        if (file != NULL)
          {
            syscall_filesys_lock_acquire ();
            size = file_length (file);
            syscall_filesys_lock_release ();
          }
        f->eax = size;
        break;
      }

    case SYS_READ:
      {
        int fd = (int) get_user_word (esp + 1);
        void *buffer = (void *) get_user_word (esp + 2);
        unsigned size = (unsigned) get_user_word (esp + 3);
        unsigned done = 0;

        if (size == 0)
          {
            f->eax = 0;
            break;
          }
        validate_user_writable_buffer (buffer, size);

        if (fd == 0)
          {
            while (done < size)
              {
                uint8_t c = input_getc ();
                copy_out ((uint8_t *) buffer + done, &c, 1);
                done++;
              }
            f->eax = done;
          }
        else
          {
            struct file *file = get_file_from_fd (fd);

            if (file == NULL)
              {
                f->eax = -1;
                break;
              }

            while (done < size)
              {
                uint8_t chunk[128];
                unsigned chunk_size = size - done;
                int bytes_read;

                if (chunk_size > sizeof chunk)
                  chunk_size = sizeof chunk;

                syscall_filesys_lock_acquire ();
                bytes_read = file_read (file, chunk, chunk_size);
                syscall_filesys_lock_release ();

                if (bytes_read <= 0)
                  break;

                copy_out ((uint8_t *) buffer + done, chunk, bytes_read);
                done += bytes_read;
                if ((unsigned) bytes_read < chunk_size)
                  break;
              }
            f->eax = done;
          }
        break;
      }

    case SYS_WRITE:
      {
        int fd = (int) get_user_word (esp + 1);
        const void *buffer = (const void *) get_user_word (esp + 2);
        unsigned size = (unsigned) get_user_word (esp + 3);
        unsigned done = 0;

        if (size == 0)
          {
            f->eax = 0;
            break;
          }
        validate_user_buffer (buffer, size);

        if (fd == 1)
          {
            while (done < size)
              {
                uint8_t chunk[256];
                unsigned chunk_size = size - done;

                if (chunk_size > sizeof chunk)
                  chunk_size = sizeof chunk;
                copy_in (chunk, (const uint8_t *) buffer + done, chunk_size);
                putbuf ((const char *) chunk, chunk_size);
                done += chunk_size;
              }
            f->eax = done;
          }
        else
          {
            struct file *file = get_file_from_fd (fd);

            if (file == NULL)
              {
                f->eax = -1;
                break;
              }

            while (done < size)
              {
                uint8_t chunk[128];
                unsigned chunk_size = size - done;
                int bytes_written;

                if (chunk_size > sizeof chunk)
                  chunk_size = sizeof chunk;
                copy_in (chunk, (const uint8_t *) buffer + done, chunk_size);

                syscall_filesys_lock_acquire ();
                bytes_written = file_write (file, chunk, chunk_size);
                syscall_filesys_lock_release ();

                if (bytes_written <= 0)
                  break;
                done += bytes_written;
                if ((unsigned) bytes_written < chunk_size)
                  break;
              }
            f->eax = done;
          }
        break;
      }

    case SYS_SEEK:
      {
        int fd = (int) get_user_word (esp + 1);
        unsigned position = (unsigned) get_user_word (esp + 2);
        struct file *file = get_file_from_fd (fd);

        if (file != NULL)
          {
            syscall_filesys_lock_acquire ();
            file_seek (file, position);
            syscall_filesys_lock_release ();
          }
        break;
      }

    case SYS_TELL:
      {
        int fd = (int) get_user_word (esp + 1);
        struct file *file = get_file_from_fd (fd);
        unsigned position = 0;

        if (file != NULL)
          {
            syscall_filesys_lock_acquire ();
            position = file_tell (file);
            syscall_filesys_lock_release ();
          }
        f->eax = position;
        break;
      }

    case SYS_CLOSE:
      close_fd ((int) get_user_word (esp + 1));
      break;

    case SYS_MMAP:
      {
        int fd = (int) get_user_word (esp + 1);
        void *addr = (void *) get_user_word (esp + 2);
        struct thread *cur = thread_current ();

        if (addr == NULL || pg_ofs (addr) != 0 || !is_user_vaddr (addr))
          {
            f->eax = MAP_FAILED;
            break;
          }

        struct file *file = get_file_from_fd (fd);
        if (file == NULL)
          {
            f->eax = MAP_FAILED;
            break;
          }

        syscall_filesys_lock_acquire ();
        struct file *rfile = file_reopen (file);
        if (rfile == NULL)
          {
            syscall_filesys_lock_release ();
            f->eax = MAP_FAILED;
            break;
          }
        off_t length = file_length (rfile);
        if (length <= 0)
          {
            file_close (rfile);
            syscall_filesys_lock_release ();
            f->eax = MAP_FAILED;
            break;
          }
        syscall_filesys_lock_release ();

        size_t page_cnt = (length + PGSIZE - 1) / PGSIZE;

        /* Check for overlap with existing mappings or pages. */
        size_t i;
        bool bad = false;
        for (i = 0; i < page_cnt; i++)
          {
            void *uaddr = addr + i * PGSIZE;
            if (!is_user_vaddr (uaddr) || pagedir_get_page (cur->pagedir, uaddr) != NULL ||
                sup_page_table_lookup (&cur->spt, uaddr) != NULL)
              {
                bad = true;
                break;
              }
          }
        if (bad)
          {
            file_close (rfile);
            f->eax = MAP_FAILED;
            break;
          }

        /* Create mmap region record. */
        struct mmap_region *m = malloc (sizeof *m);

        if (m == NULL)
          {
            file_close (rfile);
            f->eax = MAP_FAILED;
            break;
          }

        m->addr = addr;
        m->length = length;
        m->file = rfile;
        m->mapid = cur->next_mapid++;

        list_push_back (&cur->mmap_list, &m->elem);

        /* Insert supplemental page entries for the mapping. */
        for (i = 0; i < page_cnt; i++)
          {
            struct sup_page_entry *spe = malloc (sizeof *spe);
            if (spe == NULL)
              break;
            spe->user_vaddr = addr + i * PGSIZE;
            spe->access_time = 0;
            spe->flags = SUP_PAGE_WRITABLE;
            spe->file = m->file;
            spe->offset = i * PGSIZE;
            spe->read_bytes = (size_t) (length > (off_t) (i * PGSIZE) ?
                                        (size_t) (length - (off_t) (i * PGSIZE)) : 0);
            if (spe->read_bytes > PGSIZE)
              spe->read_bytes = PGSIZE;
            spe->zero_bytes = PGSIZE - spe->read_bytes;
            spe->swap_slot = 0;

            if (!sup_page_table_insert (&cur->spt, spe))
              {
                free (spe);
                break;
              }
          }

        /* If we failed to insert for some page, roll back. */
        if (i < page_cnt)
          {
            /* remove inserted entries */
            size_t j;
            for (j = 0; j < i; j++)
              {
                struct sup_page_entry *spe = sup_page_table_lookup (&cur->spt, addr + j * PGSIZE);
                if (spe != NULL)
                  sup_page_table_remove (&cur->spt, spe);
              }
            list_remove (&m->elem);
            file_close (m->file);
            free (m);
            f->eax = MAP_FAILED;
            break;
          }

        f->eax = m->mapid;
        break;
      }

    case SYS_MUNMAP:
      {
        int mapid = (int) get_user_word (esp + 1);
        struct thread *cur = thread_current ();

        struct list_elem *e;
        for (e = list_begin (&cur->mmap_list); e != list_end (&cur->mmap_list); )
          {
            struct mmap_region *m = list_entry (e, struct mmap_region, elem);
            e = list_next (e);
            if (m->mapid == mapid)
              {
                size_t page_cnt = (m->length + PGSIZE - 1) / PGSIZE;
                size_t i;
                for (i = 0; i < page_cnt; i++)
                  {
                    void *uaddr = m->addr + i * PGSIZE;
                    struct sup_page_entry *spe = sup_page_table_lookup (&cur->spt, uaddr);
                    if (spe == NULL)
                      continue;

                    void *kpage = pagedir_get_page (cur->pagedir, uaddr);
                    if (kpage != NULL)
                      {
                        if (pagedir_is_dirty (cur->pagedir, uaddr) && spe->file != NULL)
                          {
                            syscall_filesys_lock_acquire ();
                            file_write_at (spe->file, kpage, spe->read_bytes, spe->offset);
                            syscall_filesys_lock_release ();
                          }
                        pagedir_clear_page (cur->pagedir, uaddr);
                        frame_free (pg_round_down (kpage));
                      }
                    else if (spe->flags & SUP_PAGE_SWAPPED)
                      {
                        swap_free ((int) spe->swap_slot);
                      }

                    sup_page_table_remove (&cur->spt, spe);
                  }

                syscall_filesys_lock_acquire ();
                file_allow_write (m->file);
                file_close (m->file);
                syscall_filesys_lock_release ();

                list_remove (&m->elem);
                free (m);
                break;
              }
          }
        break;
      }

    default:
      process_exit_with_status (-1);
      break;
    }
}
