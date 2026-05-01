// user/vmtest.c
// PA3 test: verify page faults, eviction, swap-in, scheduler-aware eviction.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/vmstats.h"

#define PGSZ 4096
// Allocate more pages than NFRAMES (32) to force eviction.
#define NPAGES 48

static void
print_stats(char *label)
{
  struct vmstats s;
  if(getvmstats(getpid(), &s) < 0){
    printf("%s: getvmstats failed\n", label);
    return;
  }
  printf("%s: faults=%d evicted=%d swapped_out=%d swapped_in=%d resident=%d\n",
         label,
         s.page_faults, s.pages_evicted,
         s.pages_swapped_out, s.pages_swapped_in, s.resident_pages);
}

int
main(void)
{
  printf("PA3 vmtest start\n");

  // ── Test 1: Allocate NPAGES pages, touch each once ───────────────────────
  // First 32 fit in frames. Pages 33-48 force eviction of earlier pages.
  char *mem = sbrk(NPAGES * PGSZ);
  if(mem == (char*)-1){ printf("sbrk failed\n"); exit(1); }

  for(int i = 0; i < NPAGES; i++)
    mem[i * PGSZ] = (char)i; // one write per page → page fault each time

  printf("After allocating %d pages (> 32 frame limit):\n", NPAGES);
  print_stats("post-alloc");

  // ── Test 2: Re-read first 8 pages (should be in swap → swap-in) ─────────
  printf("\nRe-reading first 8 pages (expects swap-in):\n");
  volatile char sink = 0;
  for(int i = 0; i < 8; i++)
    sink += mem[i * PGSZ]; // read triggers swap-in for evicted pages
  print_stats("post-swapin");

  // ── Test 3: Fork — child has lower MLFQ priority, should lose pages first ─
  printf("\nFork test:\n");
  int cpid = fork();
  if(cpid == 0){
    // Child: try to allocate 20 more pages on top of already-full memory.
    char *more = sbrk(20 * PGSZ);
    for(int i = 0; i < 20; i++)
      more[i * PGSZ] = 1;
    print_stats("child post-alloc");
    exit(0);
  } else {
    wait(0);
    print_stats("parent after child exit");
  }

  printf("PA3 vmtest done\n");
  exit(0);
}