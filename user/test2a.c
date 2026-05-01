#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main()
{
  printf("Testing getlevel() syscall\n");

  for(int i = 0; i < 20; i++){
    printf("Current level: %d\n", getlevel());
    pause(10);
  }

  exit(0);
}