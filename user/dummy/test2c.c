#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

struct mlfqinfo info;

int
main()
{
  int pid = fork();

  if(pid == 0){
    while(1){
      getpid();
    }
  }

  pause(200);

  if(getmlfqinfo(pid, &info) == 0){
    printf("Syscall-heavy process statistics\n");
    printf("Level: %d\n", info.level);
    printf("Ticks: %d %d %d %d\n",
           info.ticks[0], info.ticks[1],
           info.ticks[2], info.ticks[3]);
    printf("Syscalls: %d\n", info.total_syscalls);
  }

  kill(pid);
  wait(0);
  exit(0);
}