// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "frame.h"
#define NFRAMES ((PHYSTOP - KERNBASE) / PGSIZE)
// #define MAX_SWAP_PAGES 16384

void freerange(void *pa_start, void *pa_end);
// int swap_out(uint64 pa);
// void swap_in(uint64 pa, int idx);
static int clock_hand = 0;

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;


// Swap space (in-memory)
// char swap_space[MAX_SWAP_PAGES][PGSIZE];
// int swap_used[MAX_SWAP_PAGES];

// struct spinlock swap_lock;

// Global frame table
struct frame_entry ft[NFRAMES];
struct spinlock frame_table_lock;

static inline int pa_to_index(uint64 pa)
{
  return (pa - KERNBASE) / PGSIZE;
}

void
kinit()
{
    initlock(&kmem.lock, "kmem");
    initlock(&frame_table_lock, "frame_table");
    // swap_lock and swap_used[] removed — handled by swap_disk.c

    for(int i = 0; i < NFRAMES; i++){
        ft[i].in_use = 0;
        ft[i].owner  = 0;
        ft[i].va     = 0;
        ft[i].ref_bit = 0;
    }

    freerange(end, (void*)PHYSTOP);
    swap_disk_init();   // initialise disk-backed swap
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// void
// kfree(void *pa)
// {
//   acquire(&frame_table_lock);

// int idx = pa_to_index((uint64)pa);
// if(ft[idx].in_use){
//   ft[idx].in_use = 0;
//   ft[idx].owner = 0;
//   ft[idx].va = 0;
//   ft[idx].ref_bit = 0;
// }

// release(&frame_table_lock);
//   struct run *r;

//   if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
//     panic("kfree");

//   // Fill with junk to catch dangling refs.
//   memset(pa, 1, PGSIZE);

//   r = (struct run*)pa;

//   acquire(&kmem.lock);
//   r->next = kmem.freelist;
//   kmem.freelist = r;
//   release(&kmem.lock);
// }

void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&frame_table_lock);
  int idx = pa_to_index((uint64)pa);
  if(ft[idx].in_use){
    // Decrement without p->lock — same pattern as the eviction path
    // to avoid AB-BA deadlock with frame_table_lock and p->lock
    if(ft[idx].owner != 0)
      ft[idx].owner->resident_pages--;
    ft[idx].in_use  = 0;
    ft[idx].owner   = 0;
    ft[idx].va      = 0;
    ft[idx].ref_bit = 0;
  }
  release(&frame_table_lock);

  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);

  // normal case
  if((r = kmem.freelist) != 0){
    kmem.freelist = r->next;
    release(&kmem.lock);

    memset((char*)r, 5, PGSIZE);

    // frame table update
    acquire(&frame_table_lock);
    int idx = pa_to_index((uint64)r);
    ft[idx].in_use = 1;
    ft[idx].owner = 0;
    ft[idx].va = 0;
    ft[idx].ref_bit = 1;
    release(&frame_table_lock);

    return (void*)r;
  }

  release(&kmem.lock);
  // printf("ENTERING EVICTION PATH\n");

  acquire(&frame_table_lock);

int victim = -1;
int victim_level = -1;

while(victim == -1){
    int start_hand = clock_hand;
    int evictable_count = 0;

    do {
        int idx = clock_hand;
        clock_hand = (clock_hand + 1) % NFRAMES;

        if(!ft[idx].in_use || ft[idx].owner == 0) continue;

        struct proc *op = ft[idx].owner;
        if(op->state == UNUSED || op->state == ZOMBIE) continue;

        evictable_count++;

        pte_t *pte = walk(op->pagetable, ft[idx].va, 0);
        if(pte && (*pte & PTE_A)){
            ft[idx].ref_bit = 1;
            *pte &= ~PTE_A;
        }

        if(ft[idx].ref_bit == 1){
            ft[idx].ref_bit = 0;
        } else {
            int prio = op->level;
            if(prio > victim_level){
                victim = idx;
                victim_level = prio;
            }
        }
    } while(clock_hand != start_hand);

    if(victim != -1) break;
    if(evictable_count == 0){
        release(&frame_table_lock);
        panic("OOM: no user pages to evict");
    }
}

struct proc *owner = ft[victim].owner;
uint64 va = ft[victim].va;
uint64 pa = KERNBASE + (uint64)victim * PGSIZE;

pte_t *pte = walk(owner->pagetable, va, 0);
if(pte == 0 || (*pte & PTE_V) == 0){
    release(&frame_table_lock);
    panic("invalid pte during eviction");
}

// Save permission bits then immediately invalidate the PTE.
// This prevents the owner from accessing the page while disk I/O is in flight.
uint64 old_flags = *pte & 0x3FF;
*pte = old_flags & ~PTE_V;
sfence_vma();

// Clear frame table entry (prevents double-eviction by Clock algorithm).
// The physical page is NOT returned to freelist yet — it is still occupied.
ft[victim].in_use = 0;
ft[victim].owner  = 0;
ft[victim].va     = 0;
ft[victim].ref_bit = 0;

// Update stats without p->lock (same convention as rest of eviction path).
owner->resident_pages--;
owner->pages_swapped_out++;
owner->pages_evicted++;

// CRITICAL: release spinlock BEFORE disk I/O.
// bread()/bwrite() acquire sleeplocks internally and may sleep.
// Calling them while holding a spinlock causes "sched locks" panic.
release(&frame_table_lock);

// Write victim page to disk — safe to sleep here (no spinlock held).
int swap_idx = disk_swap_out(pa);
if(swap_idx < 0)
    panic("swap full");

// Record the swap slot in the PTE.
// No lock needed: PTE_V is already 0 so the owner cannot access this page.
*pte = (old_flags & ~PTE_V) | PTE_S | SET_SWAP_IDX(swap_idx);
sfence_vma();

// Reuse the physical frame for the new allocation.
char *mem = (char*)pa;
memset(mem, 5, PGSIZE);

acquire(&frame_table_lock);
int idx = pa_to_index(pa);
ft[idx].in_use  = 1;
ft[idx].owner   = 0;
ft[idx].va      = 0;
ft[idx].ref_bit = 1;
release(&frame_table_lock);

return (void*)mem;
}

// int
// swap_out(uint64 pa)
// {
//   acquire(&swap_lock);

//   for(int i = 0; i < MAX_SWAP_PAGES; i++){
//     if(!swap_used[i]){
//       swap_used[i] = 1;
//       memmove(swap_space[i], (char*)pa, PGSIZE);
//       release(&swap_lock);
//       return i;
//     }
//   }

//   release(&swap_lock);
//   return -1;
// }

// void
// swap_in(uint64 pa, int idx)
// {
//   acquire(&swap_lock);

//   memmove((char*)pa, swap_space[idx], PGSIZE);

//   swap_used[idx] = 0;

//   release(&swap_lock);
// }

// void
// swap_free(int idx)
// {
//   acquire(&swap_lock);
//   swap_used[idx] = 0;
//   release(&swap_lock);
// }

// void
// swap_read(uint64 pa, int idx)
// {
//   acquire(&swap_lock);
//   memmove((char*)pa, swap_space[idx], PGSIZE);
//   release(&swap_lock);
// }