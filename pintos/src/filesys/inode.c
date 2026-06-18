#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define DIRECT_BLOCK_CNT 10
#define INDIRECT_BLOCK_CNT (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))
#define MAX_FILE_SECTORS \
  (DIRECT_BLOCK_CNT + INDIRECT_BLOCK_CNT + INDIRECT_BLOCK_CNT * INDIRECT_BLOCK_CNT)

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t direct[DIRECT_BLOCK_CNT]; /* Direct data sectors. */
    block_sector_t indirect;            /* Sector of indirect block. */
    block_sector_t doubly_indirect;     /* Sector of double-indirect block. */
    off_t length;                       /* File size in bytes. */
    int flags;                          /* Flags. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[113];               /* Not used. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

static bool inode_extend (struct inode *, off_t);
static bool inode_disk_extend (struct inode_disk *, off_t);
static block_sector_t byte_to_sector (const struct inode *, off_t);
static block_sector_t index_to_sector (const struct inode_disk *, size_t);
static bool allocate_indexed_sector (struct inode_disk *, size_t);
static bool allocate_zeroed_sector (block_sector_t *);
static void zero_sector (block_sector_t);
static void release_inode_blocks (const struct inode_disk *);
static void release_new_allocations (const struct inode_disk *,
                                     const struct inode_disk *, size_t, size_t);
static void read_indirect (block_sector_t, block_sector_t[INDIRECT_BLOCK_CNT]);
static void write_indirect (block_sector_t,
                            const block_sector_t[INDIRECT_BLOCK_CNT]);

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
    return index_to_sector (&inode->data, pos / BLOCK_SECTOR_SIZE);
  return (block_sector_t) -1;
}

static void
zero_sector (block_sector_t sector)
{
  static char zeros[BLOCK_SECTOR_SIZE];
  cache_write (sector, zeros);
}

static bool
allocate_zeroed_sector (block_sector_t *sectorp)
{
  if (!free_map_allocate (1, sectorp))
    return false;
  zero_sector (*sectorp);
  return true;
}

static void
read_indirect (block_sector_t sector, block_sector_t block[INDIRECT_BLOCK_CNT])
{
  cache_read (sector, block);
}

static void
write_indirect (block_sector_t sector,
                const block_sector_t block[INDIRECT_BLOCK_CNT])
{
  cache_write (sector, block);
}

static block_sector_t
index_to_sector (const struct inode_disk *disk_inode, size_t index)
{
  block_sector_t block[INDIRECT_BLOCK_CNT];
  block_sector_t indirect_sector;
  size_t outer;
  size_t inner;

  if (index >= MAX_FILE_SECTORS)
    return (block_sector_t) -1;

  if (index < DIRECT_BLOCK_CNT)
    return disk_inode->direct[index];
  index -= DIRECT_BLOCK_CNT;

  if (index < INDIRECT_BLOCK_CNT)
    {
      if (disk_inode->indirect == 0)
        return (block_sector_t) -1;
      read_indirect (disk_inode->indirect, block);
      return block[index];
    }
  index -= INDIRECT_BLOCK_CNT;

  if (disk_inode->doubly_indirect == 0)
    return (block_sector_t) -1;

  outer = index / INDIRECT_BLOCK_CNT;
  inner = index % INDIRECT_BLOCK_CNT;
  read_indirect (disk_inode->doubly_indirect, block);
  indirect_sector = block[outer];
  if (indirect_sector == 0)
    return (block_sector_t) -1;

  read_indirect (indirect_sector, block);
  return block[inner];
}

static bool
allocate_indexed_sector (struct inode_disk *disk_inode, size_t index)
{
  block_sector_t block[INDIRECT_BLOCK_CNT];
  block_sector_t indirect_sector;
  size_t outer;
  size_t inner;

  if (index >= MAX_FILE_SECTORS)
    return false;

  if (index < DIRECT_BLOCK_CNT)
    {
      if (disk_inode->direct[index] == 0
          && !allocate_zeroed_sector (&disk_inode->direct[index]))
        return false;
      return true;
    }
  index -= DIRECT_BLOCK_CNT;

  if (index < INDIRECT_BLOCK_CNT)
    {
      if (disk_inode->indirect == 0
          && !allocate_zeroed_sector (&disk_inode->indirect))
        return false;

      read_indirect (disk_inode->indirect, block);
      if (block[index] == 0)
        {
          if (!allocate_zeroed_sector (&block[index]))
            return false;
          write_indirect (disk_inode->indirect, block);
        }
      return true;
    }
  index -= INDIRECT_BLOCK_CNT;

  if (disk_inode->doubly_indirect == 0
      && !allocate_zeroed_sector (&disk_inode->doubly_indirect))
    return false;

  outer = index / INDIRECT_BLOCK_CNT;
  inner = index % INDIRECT_BLOCK_CNT;
  read_indirect (disk_inode->doubly_indirect, block);
  indirect_sector = block[outer];
  if (indirect_sector == 0)
    {
      if (!allocate_zeroed_sector (&indirect_sector))
        return false;
      block[outer] = indirect_sector;
      write_indirect (disk_inode->doubly_indirect, block);
    }

  read_indirect (indirect_sector, block);
  if (block[inner] == 0)
    {
      if (!allocate_zeroed_sector (&block[inner]))
        return false;
      write_indirect (indirect_sector, block);
    }
  return true;
}

static bool
inode_disk_extend (struct inode_disk *disk_inode, off_t length)
{
  struct inode_disk original = *disk_inode;
  size_t old_sectors = bytes_to_sectors (disk_inode->length);
  size_t new_sectors = bytes_to_sectors (length);
  size_t i;

  if (length < 0 || new_sectors > MAX_FILE_SECTORS)
    return false;

  for (i = old_sectors; i < new_sectors; i++)
    {
      if (!allocate_indexed_sector (disk_inode, i))
        {
          release_new_allocations (&original, disk_inode, old_sectors, i + 1);
          *disk_inode = original;
          return false;
        }
    }

  disk_inode->length = length;
  return true;
}

static bool
inode_extend (struct inode *inode, off_t length)
{
  if (length <= inode->data.length)
    return true;
  if (!inode_disk_extend (&inode->data, length))
    return false;
  cache_write (inode->sector, &inode->data);
  return true;
}

static void
release_sector_if_allocated (block_sector_t sector)
{
  if (sector != 0)
    free_map_release (sector, 1);
}

static void
release_inode_blocks (const struct inode_disk *disk_inode)
{
  block_sector_t block[INDIRECT_BLOCK_CNT];
  block_sector_t indirect[INDIRECT_BLOCK_CNT];
  size_t sector_cnt = bytes_to_sectors (disk_inode->length);
  size_t index;

  for (index = 0; index < sector_cnt && index < DIRECT_BLOCK_CNT; index++)
    release_sector_if_allocated (disk_inode->direct[index]);

  if (disk_inode->indirect != 0)
    {
      read_indirect (disk_inode->indirect, block);
      for (index = 0; index < INDIRECT_BLOCK_CNT; index++)
        release_sector_if_allocated (block[index]);
      free_map_release (disk_inode->indirect, 1);
    }

  if (disk_inode->doubly_indirect != 0)
    {
      read_indirect (disk_inode->doubly_indirect, indirect);
      for (index = 0; index < INDIRECT_BLOCK_CNT; index++)
        {
          if (indirect[index] != 0)
            {
              size_t inner;

              read_indirect (indirect[index], block);
              for (inner = 0; inner < INDIRECT_BLOCK_CNT; inner++)
                release_sector_if_allocated (block[inner]);
              free_map_release (indirect[index], 1);
            }
        }
      free_map_release (disk_inode->doubly_indirect, 1);
    }
}

static block_sector_t
old_double_indirect_child (const struct inode_disk *old_inode, size_t outer)
{
  block_sector_t block[INDIRECT_BLOCK_CNT];

  if (old_inode->doubly_indirect == 0)
    return 0;

  read_indirect (old_inode->doubly_indirect, block);
  return block[outer];
}

static void
release_new_allocations (const struct inode_disk *old_inode,
                         const struct inode_disk *new_inode,
                         size_t old_sectors, size_t new_sectors)
{
  block_sector_t indirect[INDIRECT_BLOCK_CNT];
  size_t i;

  for (i = old_sectors; i < new_sectors; i++)
    release_sector_if_allocated (index_to_sector (new_inode, i));

  if (old_inode->indirect == 0 && new_inode->indirect != 0)
    free_map_release (new_inode->indirect, 1);

  if (new_inode->doubly_indirect != 0)
    {
      read_indirect (new_inode->doubly_indirect, indirect);
      for (i = 0; i < INDIRECT_BLOCK_CNT; i++)
        {
          if (indirect[i] != 0 && old_double_indirect_child (old_inode, i) == 0)
            free_map_release (indirect[i], 1);
        }

      if (old_inode->doubly_indirect == 0)
        free_map_release (new_inode->doubly_indirect, 1);
    }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, int flags)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = 0;
      disk_inode->flags = flags;
      disk_inode->magic = INODE_MAGIC;
      if (inode_disk_extend (disk_inode, length))
        {
          cache_write (sector, disk_inode);
          success = true; 
        } 
      free (disk_inode);
    }
  return success;
}

bool
inode_is_dir (const struct inode *inode)
{
  ASSERT (inode != NULL);
  return !!(inode->data.flags & INODE_FLAGS_IS_DIR);
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  cache_read (inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

int
inode_open_count (const struct inode *inode)
{
  ASSERT (inode != NULL);
  return inode->open_cnt;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          release_inode_blocks (&inode->data);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          cache_read (sector_idx, buffer + bytes_read);
          cache_read_ahead (byte_to_sector (inode, offset + BLOCK_SECTOR_SIZE));
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          cache_read (sector_idx, bounce);
          cache_read_ahead (byte_to_sector (inode, offset + BLOCK_SECTOR_SIZE));
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  if (size > 0 && offset + size > inode->data.length)
    if (!inode_extend (inode, offset + size))
      return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          cache_write (sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            cache_read (sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          cache_write (sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
