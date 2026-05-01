#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

struct mlfqinfo info;

int
main()
{
  int pids[4];

  for(int i=0;i<4;i++){
    pids[i] = fork();
    if(pids[i] == 0){
      while(1){
      }
    }
  }

  pause(500);

  for(int i=0;i<4;i++){
    if(getmlfqinfo(pids[i], &info) == 0){
      printf("PID %d level %d\n", pids[i], info.level);
    }
  }

  for(int i=0;i<4;i++){
    kill(pids[i]);
    wait(0);
  }

  exit(0);
}