#include "kernel/types.h"
#include "user/user.h"

// struct mlfqinfo {
//   int level;
//   int ticks[4];
//   int times_scheduled;
//   int total_syscalls;
// };

void printinfo(int pid)
{
  struct mlfqinfo info;

  if(getmlfqinfo(pid, &info) == 0){
    printf("PID %d | L:%d | Ticks[%d,%d,%d,%d] | Sched:%d | Sys:%d\n",
      pid,
      info.level,
      info.ticks[0],
      info.ticks[1],
      info.ticks[2],
      info.ticks[3],
      info.times_scheduled,
      info.total_syscalls);
  }
}

void cpu_burst(long n)
{
  for(volatile long i = 0; i < n; i++);
}

int main()
{
  int st;

  printf("\n========== FINAL SC-MLFQ STRESS TEST ==========\n");

  /*
  ------------------------------------------
  CHILD 1: PURE CPU BOUND
  Should demote: L0→L1→L2→L3
  ------------------------------------------
  */
  if(fork() == 0){
    int pid = getpid();

    printf("\n[CPU] PID %d starting CPU bursts\n", pid);

    for(int i=0;i<5;i++){
      cpu_burst(400000000);
      printinfo(pid);
    }

    printf("[CPU] Final Level: %d\n", getlevel());
    exit(0);
  }

  wait(&st);

  /*
  ------------------------------------------
  CHILD 2: INTERACTIVE PROCESS
  Should stay at L0
  ------------------------------------------
  */
  if(fork() == 0){
    int pid = getpid();

    printf("\n[INTERACTIVE] PID %d starting syscall storm\n", pid);

    for(int i=0;i<5000;i++){

      getpid();

      if(i % 1000 == 0){
        printinfo(pid);
      }
    }

    printf("[INTERACTIVE] Final Level: %d\n", getlevel());
    exit(0);
  }

  wait(&st);

  /*
  ------------------------------------------
  CHILD 3: MIXED WORKLOAD
  CPU + SYSCALL PHASES
  ------------------------------------------
  */
  if(fork() == 0){

    int pid = getpid();

    printf("\n[MIXED] PID %d starting\n", pid);

    for(int phase=0; phase<5; phase++){

      cpu_burst(250000000);

      for(int j=0;j<100;j++)
        getpid();

      printinfo(pid);
    }

    printf("[MIXED] Final Level: %d\n", getlevel());
    exit(0);
  }

  wait(&st);

  /*
  ------------------------------------------
  CHILD 4 + 5: ROUND ROBIN FAIRNESS TEST
  ------------------------------------------
  */

  if(fork() == 0){

    int pid = getpid();

    printf("\n[RR-A] PID %d\n", pid);

    for(int i=0;i<3;i++){
      cpu_burst(200000000);
      printinfo(pid);
    }

    exit(0);
  }

  if(fork() == 0){

    int pid = getpid();

    printf("\n[RR-B] PID %d\n", pid);

    for(int i=0;i<3;i++){
      cpu_burst(200000000);
      printinfo(pid);
    }

    exit(0);
  }

  wait(&st);
  wait(&st);

  /*
  ------------------------------------------
  CHILD 6 + 7
  PREEMPTION TEST
  ------------------------------------------
  */

  if(fork() == 0){

    int pid = getpid();

    printf("\n[LOW PRIORITY CPU HOG] PID %d\n", pid);

    cpu_burst(500000000);

    printinfo(pid);

    cpu_burst(500000000);

    printinfo(pid);

    exit(0);
  }

  if(fork() == 0){

    int pid = getpid();

    printf("\n[HIGH PRIORITY INTERACTIVE] PID %d\n", pid);

    pause(1);

    for(int i=0;i<1000;i++)
      getpid();

    printinfo(pid);

    exit(0);
  }

  wait(&st);
  wait(&st);

  /*
  ------------------------------------------
  CHILD 8
  BOOST TEST
  ------------------------------------------
  */

  if(fork() == 0){

    int pid = getpid();

    printf("\n[BOOST TEST] PID %d\n", pid);

    for(int i=0;i<10;i++){

      cpu_burst(300000000);

      printinfo(pid);
    }

    printf("[BOOST TEST] Final Level: %d\n", getlevel());

    exit(0);
  }

  wait(&st);

  printf("\n========== TEST COMPLETE ==========\n");

  exit(0);
}