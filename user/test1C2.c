#include "kernel/types.h"
#include "user/user.h"

int main(){
    int c1 = getsyscount();
    printf("System calls count initial: %d\n",c1);
    getpid();
    getpid2();
    pause(5);
    int c2 = getsyscount();
    printf("System calls count after some system calls: %d\n",c2);
    return 0;
}

