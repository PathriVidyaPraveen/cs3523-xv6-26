#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void print_stats(int pid) {
  struct vmstats s;
  if(getvmstats(pid, &s) < 0){
    printf("getvmstats failed for pid %d\n", pid);
    return;
  }

  printf("[PID %d] PF=%d Evict=%d SwapIn=%d SwapOut=%d Resident=%d\n",
    pid, s.page_faults, s.pages_evicted,
    s.pages_swapped_in, s.pages_swapped_out,
    s.resident_pages);
}

// allocate N pages and touch them
void alloc_and_touch(int pages) {
  char *arr = sbrk(pages * 4096);
  for(int i = 0; i < pages * 4096; i += 4096){
    arr[i] = 1;  // trigger lazy alloc
  }
}

// force swap by accessing large memory repeatedly
void thrash_memory(int pages) {
  char *arr = sbrk(pages * 4096);
  for(int round = 0; round < 5; round++){
    for(int i = 0; i < pages * 4096; i += 4096){
      arr[i] += 1;
    }
  }
}

// alternating access pattern
void zigzag_access(int pages) {
  char *arr = sbrk(pages * 4096);

  for(int i = 0; i < pages; i++){
    arr[i * 4096] = 1;
  }

  for(int i = pages - 1; i >= 0; i--){
    arr[i * 4096] += 1;
  }
}

// random-ish pattern
void random_access(int pages) {
  char *arr = sbrk(pages * 4096);

  for(int i = 0; i < pages * 10; i++){
    int idx = (i * 37) % pages;
    arr[idx * 4096] += 1;
  }
}

// edge: access last valid byte
void boundary_test() {
  char *arr = sbrk(4096);
  arr[4095] = 42;
}

// edge: invalid access (should kill child)
void invalid_access_test() {
  int pid = fork();
  if(pid == 0){
    char *bad = (char*)0xFFFFFFFFFFFF; // invalid VA
    *bad = 1;
    exit(0);
  } else {
    wait(0);
    printf("Invalid access test done\n");
  }
}

// permission fault test (write to read-only)
void permission_fault_test() {
  int pid = fork();
  if(pid == 0){
    char *arr = sbrk(4096);
    arr[0] = 1;

    // simulate read-only by unmapping + remapping without write (if your OS allows)
    // otherwise just try illegal write pattern
    *(char*)arr = 5;

    exit(0);
  } else {
    wait(0);
    printf("Permission fault test done\n");
  }
}

// multi-process stress
void multi_process_test() {
  for(int i = 0; i < 4; i++){
    int pid = fork();
    if(pid == 0){
      alloc_and_touch(30);
      thrash_memory(30);
      print_stats(getpid());
      exit(0);
    }
  }

  for(int i = 0; i < 4; i++)
    wait(0);
}

// heavy eviction test
void heavy_eviction_test() {
  printf("Running heavy eviction test...\n");
alloc_and_touch(1000);
thrash_memory(1000);
  zigzag_access(100);
  random_access(100);
}

// fork inheritance test
void fork_test() {
  int pid = fork();
  if(pid == 0){
    alloc_and_touch(20);
    print_stats(getpid());
    exit(0);
  } else {
    wait(0);
    print_stats(getpid());
  }
}

// reuse freed memory
void reuse_test() {
  char *a = sbrk(10 * 4096);
  for(int i = 0; i < 10; i++)
    a[i * 4096] = 1;

  sbrk(-10 * 4096);

  char *b = sbrk(10 * 4096);
  for(int i = 0; i < 10; i++)
    b[i * 4096] = 2;
}

// extreme stress
void extreme_test() {
  printf("Running extreme stress test...\n");

  for(int i = 0; i < 5; i++){
    alloc_and_touch(80);
    thrash_memory(80);
    zigzag_access(80);
    random_access(80);
  }
}

int main() {
  printf("===== VM STRESS TEST START =====\n");

  printf("\n--- Basic Allocation ---\n");
  alloc_and_touch(10);
  print_stats(getpid());

  printf("\n--- Boundary Test ---\n");
  boundary_test();

  printf("\n--- Zigzag Access ---\n");
  zigzag_access(20);
  print_stats(getpid());

  printf("\n--- Random Access ---\n");
  random_access(20);
  print_stats(getpid());

  printf("\n--- Heavy Eviction ---\n");
  heavy_eviction_test();
  print_stats(getpid());

  printf("\n--- Fork Test ---\n");
  fork_test();

  printf("\n--- Multi Process Test ---\n");
  multi_process_test();

  printf("\n--- Reuse Test ---\n");
  reuse_test();

  printf("\n--- Invalid Access Test ---\n");
  invalid_access_test();

  printf("\n--- Permission Fault Test ---\n");
  permission_fault_test();

  printf("\n--- Extreme Stress ---\n");
  extreme_test();
  print_stats(getpid());

  printf("\n===== VM STRESS TEST END =====\n");

  exit(0);
}