#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "devices/block.h"
#include "filesys/file.h"

#define STACK_MAX_SIZE (128 * 1024 * 1024)

enum sup_page_type
  {
    SUP_PAGE_ANON,
    SUP_PAGE_FILE,
    SUP_PAGE_MMAP
  };

enum sup_page_flags
  {
    SUP_PAGE_NONE     = 0,
    SUP_PAGE_DIRTY    = 1 << 0,
    SUP_PAGE_ACCESSED = 1 << 1,
    SUP_PAGE_WRITABLE = 1 << 2,
    SUP_PAGE_SWAPPED  = 1 << 3,
  };

struct sup_page_entry
  {
    void *user_vaddr;

    int64_t access_time;

    enum sup_page_type type;
    int flags;

    struct hash_elem elem;

    struct file *file;
    off_t offset;
    size_t read_bytes;
    size_t zero_bytes;

    block_sector_t swap_slot;
  };

struct sup_page_table
  {
    struct hash table;
  };

void sup_page_table_init (struct sup_page_table *);
void sup_page_table_destroy (struct sup_page_table *);
bool sup_page_table_insert (struct sup_page_table *, struct sup_page_entry *);
void sup_page_table_remove (struct sup_page_table *, struct sup_page_entry *);
struct sup_page_entry *sup_page_table_lookup (struct sup_page_table *, void *user_vaddr);

struct sup_page_entry *sup_page_create (void *user_vaddr,
                                        enum sup_page_type type,
                                        bool writable);
bool sup_page_table_add_file (struct sup_page_table *, void *user_vaddr,
                              struct file *, off_t, size_t, size_t,
                              bool, enum sup_page_type);
bool sup_page_table_add_anon (struct sup_page_table *, void *user_vaddr,
                              bool writable);
bool load_page(struct sup_page_entry *);

#endif /* vm/page.h */
