#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

struct mlfqinfo info1, info2;

int
main()
{
  int pid1 = fork();

  if(pid1 == 0){
    while(1){
    }
  }

  int pid2 = fork();

  if(pid2 == 0){
    while(1){
      getpid();
    }
  }

  pause(200);

  getmlfqinfo(pid1, &info1);
  getmlfqinfo(pid2, &info2);

  printf("\nCPU-bound process level: %d\n", info1.level);
  printf("Syscall-heavy process level: %d\n", info2.level);

  kill(pid1);
  kill(pid2);

  wait(0);
  wait(0);

  exit(0);
}