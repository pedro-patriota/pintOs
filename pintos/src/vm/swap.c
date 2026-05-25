#include "vm/swap.h"
#include <debug.h>
#include <stdint.h>
#include <string.h>
#include "devices/block.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

#define NUM_SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

static struct block *swap_block = NULL;
static size_t num_slots = 0;
static char *swap_map = NULL;
static struct lock swap_lock;

void
swap_init (void)
{
  size_t num_sectors = 0;

  lock_init (&swap_lock);
  swap_block = block_get_role (BLOCK_SWAP);
  if (swap_block == NULL)
    return;

  num_sectors = block_size (swap_block);
  num_slots = num_sectors / NUM_SECTORS_PER_PAGE;
  swap_map = malloc (num_slots);
  if (swap_map == NULL)
    PANIC ("Failed to allocate swap map");
  memset (swap_map, 0, num_slots);
}

void
swap_in (int slot, void *kernel_vaddr)
{
  size_t base = (size_t)slot * NUM_SECTORS_PER_PAGE;
  size_t s;
  uint8_t *buf = kernel_vaddr;

  if (swap_block == NULL || slot < 0 || (size_t)slot >= num_slots)
    return;

  for (s = 0; s < NUM_SECTORS_PER_PAGE; ++s)
    block_read (swap_block, base + s, buf + s * BLOCK_SECTOR_SIZE);

  lock_acquire (&swap_lock);
  swap_map[slot] = 0;
  lock_release (&swap_lock);
}

int
swap_out (void *kernel_vaddr)
{
  size_t i = 0, s = 0;
  size_t base = 0;
  uint8_t *buf = kernel_vaddr;

  if (swap_block == NULL)
    return -1;

  lock_acquire (&swap_lock);
  for (i = 0; i < num_slots; i++)
    if (!swap_map[i])
      {
        swap_map[i] = 1;
        break;
      }
  lock_release (&swap_lock);

  if (i == num_slots)
    return -1;

  base = i * NUM_SECTORS_PER_PAGE;
  for (s = 0; s < NUM_SECTORS_PER_PAGE; s++)
    block_write (swap_block, base + s, buf + s * BLOCK_SECTOR_SIZE);

  return (int) i;
}

void
swap_free (int slot)
{
  if (swap_block == NULL || slot < 0 || (size_t)slot >= num_slots)
    return;
  lock_acquire (&swap_lock);
  swap_map[slot] = 0;
  lock_release (&swap_lock);
}
