#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#ifdef VM
#include "vm/frame.h"
#include "vm/page.h"
#endif

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

struct start_process_args
  {
    char *file_name;
    struct child_process *child;
  };

static void child_record_release (struct child_process *child);

static void
child_record_release (struct child_process *child)
{
  enum intr_level old_level;
  bool should_free;

  if (child == NULL)
    return;

  old_level = intr_disable ();
  ASSERT (child->ref_cnt > 0);
  child->ref_cnt--;
  should_free = child->ref_cnt == 0;
  intr_set_level (old_level);

  if (should_free)
    free (child);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name)
{
  char *fn_copy;
  char *prog_name_copy;
  struct child_process *child;
  struct start_process_args *args;
  tid_t tid;
  char *prog_name;
  char *save_ptr;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  prog_name_copy = palloc_get_page (0);
  if (prog_name_copy == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  strlcpy (prog_name_copy, file_name, PGSIZE);
  prog_name = strtok_r (prog_name_copy, " ", &save_ptr);
  if (prog_name == NULL)
    {
      palloc_free_page (prog_name_copy);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  child = malloc (sizeof *child);
  if (child == NULL)
    {
      palloc_free_page (prog_name_copy);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  child->tid = TID_ERROR;
  child->exit_status = -1;
  child->load_success = false;
  child->waited = false;
  child->ref_cnt = 2;
  sema_init (&child->load_sema, 0);
  sema_init (&child->exit_sema, 0);

  args = malloc (sizeof *args);
  if (args == NULL)
    {
      free (child);
      palloc_free_page (prog_name_copy);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  args->file_name = fn_copy;
  args->child = child;

  list_push_back (&thread_current ()->children, &child->elem);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (prog_name, PRI_DEFAULT, start_process, args);
  palloc_free_page (prog_name_copy);
  if (tid == TID_ERROR)
    {
      list_remove (&child->elem);
      free (child);
      free (args);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  child->tid = tid;
  sema_down (&child->load_sema);
  if (!child->load_success)
    {
      list_remove (&child->elem);
      child_record_release (child);
      return TID_ERROR;
    }
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  struct start_process_args *args = file_name_;
  char *file_name = args->file_name;
  struct child_process *child = args->child;
  struct intr_frame if_;
  bool success;

#ifdef VM
  sup_page_table_init (&thread_current ()->spt);
  list_init (&thread_current ()->mmap_list);
  thread_current ()->next_mapid = 1;
  thread_current ()->user_esp = NULL;
#endif

  thread_current ()->child_record = child;
  free (args);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  /* If load failed, quit. */
  palloc_free_page (file_name);
  child->load_success = success;
  sema_up (&child->load_sema);
  if (!success)
    thread_exit ();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  for (e = list_begin (&cur->children); e != list_end (&cur->children);
       e = list_next (e))
    {
      struct child_process *child =
        list_entry (e, struct child_process, elem);
      if (child->tid == child_tid)
        {
          int status;

          if (child->waited)
            return -1;

          child->waited = true;
          sema_down (&child->exit_sema);
          status = child->exit_status;
          list_remove (&child->elem);
          child_record_release (child);
          return status;
        }
    }
  return -1;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  int fd;
  struct list_elem *e;

  syscall_filesys_lock_acquire ();
  for (fd = 2; fd < MAX_FD; fd++)
    {
      if (cur->fd_table[fd] != NULL)
        {
          file_close (cur->fd_table[fd]);
          cur->fd_table[fd] = NULL;
        }
    }
  if (cur->executable != NULL)
    {
      file_close (cur->executable);
      cur->executable = NULL;
    }
  syscall_filesys_lock_release ();

  while (!list_empty (&cur->children))
    {
      e = list_pop_front (&cur->children);
      child_record_release (list_entry (e, struct child_process, elem));
    }

  if (cur->child_record != NULL)
    {
      cur->child_record->exit_status = cur->exit_status;
      sema_up (&cur->child_record->exit_sema);
      child_record_release (cur->child_record);
      cur->child_record = NULL;
    }

#ifdef VM
  /* Unmap any remaining memory-mapped files. */
  syscall_do_munmap_all ();
  sup_page_table_destroy (&cur->spt);
#endif

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL)
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

void
process_exit_with_status (int status)
{
  struct thread *cur = thread_current ();

  cur->exit_status = status;
  if (cur->pagedir != NULL)
    printf ("%s: exit(%d)\n", cur->name, status);
  thread_exit ();
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, const char *cmd_line);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);
static bool push_stack_args (void **esp, const char *cmd_line);
static bool stack_page_alloc (void *upage, uint8_t **kpage);
static void stack_page_free (void *kpage);
static void stack_page_loaded (void *kpage);
static void stack_page_remove_spt_entry (void *upage);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;
  char *prog_name_copy = NULL;
  char *prog_name;
  char *save_ptr;
  bool filesys_locked = false;

  char *cmd_line_copy = palloc_get_page (0);
  if (cmd_line_copy == NULL)
    return false;
  strlcpy (cmd_line_copy, file_name, PGSIZE);

  prog_name_copy = palloc_get_page (0);
  if (prog_name_copy == NULL)
    goto done;
  strlcpy (prog_name_copy, file_name, PGSIZE);
  prog_name = strtok_r (prog_name_copy, " ", &save_ptr);

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  /* Open executable file. */
  syscall_filesys_lock_acquire ();
  filesys_locked = true;
  file = filesys_open (prog_name);
  if (file == NULL)
    {
      printf ("load: %s: open failed\n", prog_name);
      goto done;
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

#ifdef VM
  file_deny_write (file);
  syscall_filesys_lock_release ();
  filesys_locked = false;
#endif

  /* Set up stack. */
  if (!setup_stack (esp, cmd_line_copy))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

#ifndef VM
  file_deny_write (file);
#endif
  t->executable = file;
  file = NULL;
  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  if (filesys_locked)
    {
      file_close (file);
      syscall_filesys_lock_release ();
    }
  else if (file != NULL)
    {
      syscall_filesys_lock_acquire ();
      file_close (file);
      syscall_filesys_lock_release ();
    }
  if (prog_name_copy != NULL)
    palloc_free_page (prog_name_copy);
  if (cmd_line_copy != NULL)
    palloc_free_page (cmd_line_copy);
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

#ifdef VM
  while (read_bytes > 0 || zero_bytes > 0)
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      if (!sup_page_table_add_file (&thread_current ()->spt, upage, file, ofs,
                                    page_read_bytes, page_zero_bytes, writable,
                                    SUP_PAGE_FILE))
        return false;

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      ofs += PGSIZE;
    }
#else
  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
      uint8_t *kpage = palloc_get_page (PAL_USER);

      if (kpage == NULL)
        return false;

      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
#endif
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory.  Parses CMD_LINE and pushes arguments
   onto the stack following the 80x86 calling convention. */
static bool
setup_stack (void **esp, const char *cmd_line)
{
  uint8_t *kpage;
  void *upage = ((uint8_t *) PHYS_BASE) - PGSIZE;
  bool success = false;

  if (!stack_page_alloc (upage, &kpage))
    return false;

  success = install_page (upage, kpage, true);
  if (success)
    {
      *esp = PHYS_BASE;
      success = push_stack_args (esp, cmd_line);
      if (!success)
        {
          pagedir_clear_page (thread_current ()->pagedir, upage);
          stack_page_free (kpage);
          goto cleanup;
        }
      stack_page_loaded (kpage);
    }
  else
    stack_page_free (kpage);

cleanup:
  if (!success)
    stack_page_remove_spt_entry (upage);
  return success;
}

static bool
stack_page_alloc (void *upage, uint8_t **kpage)
{
#ifdef VM
  if (!sup_page_table_add_anon (&thread_current ()->spt, upage, true))
    return false;

  *kpage = frame_alloc (upage);
  if (*kpage != NULL)
    memset (*kpage, 0, PGSIZE);
#else
  (void) upage;
  *kpage = palloc_get_page (PAL_USER | PAL_ZERO);
#endif
  return *kpage != NULL;
}

static void
stack_page_free (void *kpage)
{
#ifdef VM
  frame_free (kpage);
#else
  palloc_free_page (kpage);
#endif
}

static void
stack_page_loaded (void *kpage)
{
#ifdef VM
  frame_unpin (kpage);
#else
  (void) kpage;
#endif
}

static void
stack_page_remove_spt_entry (void *upage)
{
#ifdef VM
  struct sup_page_entry *spe =
    sup_page_table_lookup (&thread_current ()->spt, upage);
  if (spe != NULL)
    sup_page_table_remove (&thread_current ()->spt, spe);
#else
  (void) upage;
#endif
}

static bool
push_stack_args (void **esp, const char *cmd_line)
{
  char *cmd_copy;
  char *token, *save_ptr;
  char *argv[128];
  char *argv_addrs[128];
  char **argv_start;
  int argc = 0;
  int i;

  cmd_copy = palloc_get_page (0);
  if (cmd_copy == NULL)
    return false;
  strlcpy (cmd_copy, cmd_line, PGSIZE);

  for (token = strtok_r (cmd_copy, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    argv[argc++] = token;

  /* Push argument strings onto the stack (right to left). */
  for (i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1;
      *esp -= len;
      memcpy (*esp, argv[i], len);
      argv_addrs[i] = *esp;
    }

  /* Word-align. */
  *esp = (void *)((uintptr_t) *esp & ~3);

  /* Push null sentinel (argv[argc]). */
  *esp -= sizeof (char *);
  *(char **)*esp = NULL;

  /* Push argv[i] pointers (right to left). */
  for (i = argc - 1; i >= 0; i--)
    {
      *esp -= sizeof (char *);
      *(char **)*esp = argv_addrs[i];
    }

  /* Push argv (pointer to argv[0]). */
  argv_start = *esp;
  *esp -= sizeof (char **);
  *(char ***)*esp = argv_start;

  /* Push argc. */
  *esp -= sizeof (int);
  *(int *)*esp = argc;

  /* Push fake return address. */
  *esp -= sizeof (void *);
  *(void **)*esp = NULL;

#if 0
  /* Print the stack. Make sure to keep this DISABLED during tests. */
  hex_dump ((uintptr_t) *esp, *esp,
            (uintptr_t) PHYS_BASE - (uintptr_t) *esp, true);
#endif

  palloc_free_page (cmd_copy);
  return true;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
