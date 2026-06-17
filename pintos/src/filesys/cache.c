#include "filesys/cache.h"
#include <debug.h>
#include <stdbool.h>
#include <string.h>
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "threads/thread.h"

#define CACHE_ENTRY_CNT 64
#define READ_AHEAD_QUEUE_CNT 64
#define WRITE_BEHIND_INTERVAL (TIMER_FREQ)

struct cache_entry
  {
    bool valid;
    bool dirty;
    bool accessed;
    block_sector_t sector;
    uint8_t data[BLOCK_SECTOR_SIZE];
  };

static struct cache_entry cache[CACHE_ENTRY_CNT];
static struct lock cache_lock;
static size_t clock_hand;

static struct lock read_ahead_lock;
static struct semaphore read_ahead_sema;
static block_sector_t read_ahead_queue[READ_AHEAD_QUEUE_CNT];
static size_t read_ahead_head;
static size_t read_ahead_tail;
static size_t read_ahead_cnt;
static bool cache_shutting_down;

static struct cache_entry *cache_get_entry (block_sector_t);
static struct cache_entry *cache_lookup (block_sector_t);
static struct cache_entry *cache_select_victim (void);
static void cache_flush_entry (struct cache_entry *);
static void read_ahead_worker (void *);
static void write_behind_worker (void *);
static void cache_prefetch (block_sector_t);

void
cache_init (void)
{
  lock_init (&cache_lock);
  lock_init (&read_ahead_lock);
  sema_init (&read_ahead_sema, 0);
  clock_hand = 0;
  read_ahead_head = read_ahead_tail = read_ahead_cnt = 0;
  cache_shutting_down = false;
  thread_create ("fs-write-behind", PRI_DEFAULT, write_behind_worker, NULL);
  thread_create ("fs-read-ahead", PRI_DEFAULT, read_ahead_worker, NULL);
}

void
cache_done (void)
{
  cache_shutting_down = true;
  cache_flush_all ();
}

void
cache_read (block_sector_t sector, void *buffer)
{
  struct cache_entry *entry;

  ASSERT (buffer != NULL);

  lock_acquire (&cache_lock);
  entry = cache_get_entry (sector);
  memcpy (buffer, entry->data, BLOCK_SECTOR_SIZE);
  entry->accessed = true;
  lock_release (&cache_lock);
}

void
cache_write (block_sector_t sector, const void *buffer)
{
  struct cache_entry *entry;

  ASSERT (buffer != NULL);

  lock_acquire (&cache_lock);
  entry = cache_get_entry (sector);
  memcpy (entry->data, buffer, BLOCK_SECTOR_SIZE);
  entry->dirty = true;
  entry->accessed = true;
  lock_release (&cache_lock);
}

void
cache_read_ahead (block_sector_t sector)
{
  if (sector == (block_sector_t) -1 || sector >= block_size (fs_device))
    return;

  lock_acquire (&read_ahead_lock);
  if (read_ahead_cnt < READ_AHEAD_QUEUE_CNT)
    {
      read_ahead_queue[read_ahead_tail] = sector;
      read_ahead_tail = (read_ahead_tail + 1) % READ_AHEAD_QUEUE_CNT;
      read_ahead_cnt++;
      sema_up (&read_ahead_sema);
    }
  lock_release (&read_ahead_lock);
}

void
cache_flush_all (void)
{
  size_t i;

  lock_acquire (&cache_lock);
  for (i = 0; i < CACHE_ENTRY_CNT; i++)
    cache_flush_entry (&cache[i]);
  lock_release (&cache_lock);
}

static struct cache_entry *
cache_get_entry (block_sector_t sector)
{
  struct cache_entry *entry = cache_lookup (sector);

  ASSERT (sector < block_size (fs_device));

  if (entry != NULL)
    return entry;

  entry = cache_select_victim ();
  cache_flush_entry (entry);
  entry->valid = true;
  entry->dirty = false;
  entry->accessed = true;
  entry->sector = sector;
  block_read (fs_device, sector, entry->data);
  return entry;
}

static struct cache_entry *
cache_lookup (block_sector_t sector)
{
  size_t i;

  for (i = 0; i < CACHE_ENTRY_CNT; i++)
    if (cache[i].valid && cache[i].sector == sector)
      return &cache[i];
  return NULL;
}

static struct cache_entry *
cache_select_victim (void)
{
  for (;;)
    {
      struct cache_entry *entry = &cache[clock_hand];
      clock_hand = (clock_hand + 1) % CACHE_ENTRY_CNT;

      if (!entry->valid)
        return entry;

      if (entry->accessed)
        {
          entry->accessed = false;
          continue;
        }

      return entry;
    }
}

static void
cache_flush_entry (struct cache_entry *entry)
{
  if (entry->valid && entry->dirty)
    {
      block_write (fs_device, entry->sector, entry->data);
      entry->dirty = false;
    }
}

static void
cache_prefetch (block_sector_t sector)
{
  if (sector == (block_sector_t) -1 || sector >= block_size (fs_device))
    return;

  lock_acquire (&cache_lock);
  (void) cache_get_entry (sector);
  lock_release (&cache_lock);
}

static void
read_ahead_worker (void *aux UNUSED)
{
  for (;;)
    {
      block_sector_t sector;

      sema_down (&read_ahead_sema);
      if (cache_shutting_down)
        continue;

      lock_acquire (&read_ahead_lock);
      if (read_ahead_cnt == 0)
        {
          lock_release (&read_ahead_lock);
          continue;
        }
      sector = read_ahead_queue[read_ahead_head];
      read_ahead_head = (read_ahead_head + 1) % READ_AHEAD_QUEUE_CNT;
      read_ahead_cnt--;
      lock_release (&read_ahead_lock);

      cache_prefetch (sector);
    }
}

static void
write_behind_worker (void *aux UNUSED)
{
  for (;;)
    {
      timer_sleep (WRITE_BEHIND_INTERVAL);
      if (!cache_shutting_down)
        cache_flush_all ();
    }
}
