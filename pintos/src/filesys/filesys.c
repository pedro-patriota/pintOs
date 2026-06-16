#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include <stdlib.h>

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Opens a directory given its sector. */
static struct dir *
dir_open_from_sector (block_sector_t sector)
{
  struct inode *inode = inode_open (sector);
  if (inode == NULL)
    return NULL;
  return dir_open (inode);
}

/* Lookup the inode for the given path. */
static struct inode *
path_lookup (const char *path)
{
  if (path == NULL)
    return NULL;

  /* Empty string is invalid. */
  if (path[0] == '\0')
    return NULL;

  /* Special case: root. */
  if (path[0] == '/' && path[1] == '\0')
    return inode_open (ROOT_DIR_SECTOR);

  char *copy = malloc (strlen (path) + 1);
  if (copy == NULL)
    return NULL;
  strlcpy (copy, path, strlen (path) + 1);

  struct dir *dir = NULL;
  struct inode *inode = NULL;
  char *saveptr = NULL;
  char *token = NULL;

  /* Start at root for absolute paths, or at thread cwd for relative. */
  if (path[0] == '/')
    dir = dir_open_root ();
  else
    {
      block_sector_t cwd = ROOT_DIR_SECTOR;
      struct thread *t = thread_current ();
      if (t != NULL)
        cwd = t->cwd ? t->cwd : ROOT_DIR_SECTOR;
      dir = dir_open_from_sector (cwd);
    }

  if (dir == NULL)
    {
      free (copy);
      return NULL;
    }

  token = strtok_r (copy, "/", &saveptr);
  if (token == NULL)
    {
      /* Path was "/"; return root inode. */
      inode = inode_reopen (dir_get_inode (dir));
      dir_close (dir);
      free (copy);
      return inode;
    }

  for (;;)
    {
      char *next = strtok_r (NULL, "/", &saveptr);

      if (strcmp (token, ".") == 0)
        {
          /* Stay in current dir. If this is the last component, return it. */
          if (next == NULL)
            {
              inode = inode_reopen (dir_get_inode (dir));
              dir_close (dir);
              free (copy);
              return inode;
            }
        }
      else if (strcmp (token, "..") == 0)
        {
          /* Move to parent. If this is the last component, return parent inode. */
          struct inode *p_inode = NULL;
          if (!dir_lookup (dir, "..", &p_inode) || p_inode == NULL)
            {
              dir_close (dir);
              free (copy);
              return NULL;
            }
          if (next == NULL)
            {
              dir_close (dir);
              free (copy);
              return p_inode;
            }
          struct dir *p_dir = dir_open (p_inode);
          dir_close (dir);
          dir = p_dir;
        }
      else
        {
          struct inode *next_inode = NULL;
          if (!dir_lookup (dir, token, &next_inode) || next_inode == NULL)
            {
              dir_close (dir);
              free (copy);
              return NULL;
            }
          if (next == NULL)
            {
              /* Last component: return this inode. */
              dir_close (dir);
              free (copy);
              return next_inode;
            }
          /* Not last: must be a directory. */
          if (!inode_is_dir (next_inode))
            {
              inode_close (next_inode);
              dir_close (dir);
              free (copy);
              return NULL;
            }
          /* Descend into next directory. */
          struct dir *next_dir = dir_open (next_inode);
          dir_close (dir);
          dir = next_dir;
        }

      if (next == NULL)
        break;
      token = next;
    }

  /* Should not reach here. */
  dir_close (dir);
  free (copy);
  return NULL;
}

/* Helper: Given a path, return the parent directory (opened) and copy the
   final path component into NAME. Caller must close parent_dir. Returns
   true on success. */
static bool
get_parent_dir (const char *path, struct dir **parent_dir, char name[NAME_MAX + 1])
{
  if (path == NULL || parent_dir == NULL || name == NULL)
    return false;

  char *copy = malloc (strlen (path) + 1);
  if (copy == NULL)
    return false;
  strlcpy (copy, path, strlen (path) + 1);

  struct dir *dir = NULL;
  char *saveptr = NULL;
  char *token = NULL;

  /* Start at root for absolute paths, or at thread cwd for relative. */
  if (path[0] == '/')
    dir = dir_open_root ();
  else
    {
      block_sector_t cwd = ROOT_DIR_SECTOR;
      struct thread *t = thread_current ();
      if (t != NULL)
        cwd = t->cwd ? t->cwd : ROOT_DIR_SECTOR;
      dir = dir_open_from_sector (cwd);
    }

  if (dir == NULL)
    {
      free (copy);
      return false;
    }

  token = strtok_r (copy, "/", &saveptr);
  if (token == NULL)
    {
      /* Path is "/" - parent is root, name is empty. */
      *parent_dir = dir;
      name[0] = '\0';
      free (copy);
      return true;
    }

  char *next = NULL;
  for (;;)
    {
      next = strtok_r (NULL, "/", &saveptr);
      if (next == NULL)
        {
          /* token is the final component */
          if (strlen (token) > NAME_MAX)
            {
              dir_close (dir);
              free (copy);
              return false;
            }
          strlcpy (name, token, NAME_MAX + 1);
          *parent_dir = dir;
          free (copy);
          return true;
        }

      /* Intermediate component: descend into directory. */
      if (strcmp (token, ".") == 0)
        {
          /* stay */
        }
      else if (strcmp (token, "..") == 0)
        {
          struct inode *p_inode = NULL;
          if (!dir_lookup (dir, "..", &p_inode) || p_inode == NULL)
            {
              dir_close (dir);
              free (copy);
              return false;
            }
          struct dir *p_dir = dir_open (p_inode);
          dir_close (dir);
          dir = p_dir;
        }
      else
        {
          struct inode *next_inode = NULL;
          if (!dir_lookup (dir, token, &next_inode) || next_inode == NULL)
            {
              dir_close (dir);
              free (copy);
              return false;
            }
          if (!inode_is_dir (next_inode))
            {
              inode_close (next_inode);
              dir_close (dir);
              free (copy);
              return false;
            }
          struct dir *next_dir = dir_open (next_inode);
          dir_close (dir);
          dir = next_dir;
        }

      token = next;
    }
}

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  /* Create file given a possibly-path name. */
  struct dir *parent = NULL;
  char final_name[NAME_MAX + 1];
  bool success = false;
  block_sector_t inode_sector = 0;

  /* Find parent directory and final component. */
  if (!get_parent_dir (name, &parent, final_name))
    return false;

  if (parent != NULL)
    {
      if (free_map_allocate (1, &inode_sector))
        {
          if (inode_create (inode_sector, initial_size, INODE_FLAGS_NONE)
              && dir_add (parent, final_name, inode_sector))
            success = true;
          if (!success)
            free_map_release (inode_sector, 1);
        }
      dir_close (parent);
    }
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  /* Open file or directory at path NAME. */
  struct inode *inode = path_lookup (name);
  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *parent = NULL;
  char final_name[NAME_MAX + 1];
  bool success = false;

  if (!get_parent_dir (name, &parent, final_name))
    return false;
  if (parent != NULL)
    {
      success = dir_remove (parent, final_name);
      dir_close (parent);
    }
  return success;
}

bool
filesys_mkdir (const char *name)
{
  static char final_name[NAME_MAX + 1] = {0};
  struct dir *parent = NULL;
  bool success = false;

  if (!get_parent_dir (name, &parent, final_name))
    return false;

  if (parent == NULL)
    return false;

  /* Allocate inode sector for directory. */
  block_sector_t inode_sector = 0;
  if (!free_map_allocate (1, &inode_sector))
    {
      dir_close (parent);
      return false;
    }

  if (!dir_create (inode_sector, 16))
    {
      free_map_release (inode_sector, 1);
      dir_close (parent);
      return false;
    }

  /* Add entry in parent directory. */
  if (!dir_add (parent, final_name, inode_sector))
    {
      free_map_release (inode_sector, 1);
      dir_close (parent);
      return false;
    }

  /* Initialize '.' and '..' in new directory. */
  {
    struct dir *newdir = dir_open (inode_open (inode_sector));
    if (newdir != NULL)
      {
        struct inode *n_inode = dir_get_inode (newdir);
        dir_add (newdir, ".", inode_sector);
        dir_add (newdir, "..", inode_get_inumber (dir_get_inode (parent)));
        dir_close (newdir);
      }
  }

  dir_close (parent);
  success = true;
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  /* Initialize root directory '.' and '..' entries. */
  {
    struct dir *root = dir_open_root ();
    if (root != NULL)
      {
        dir_add (root, ".", ROOT_DIR_SECTOR);
        dir_add (root, "..", ROOT_DIR_SECTOR);
        dir_close (root);
      }
  }
  free_map_close ();
  printf ("done.\n");
}
