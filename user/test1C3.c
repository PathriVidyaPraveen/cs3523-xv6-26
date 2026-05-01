#include "kernel/types.h"
#include "user/user.h"

int main(){

    int pid = fork();
    if(pid==0){
        getpid();
        getpid2();
        pause(10);
        return 0;
    }
    pause(2);
    int c = getchildsyscount(pid);
    printf("Child syscount (pid = %d): %d\n",pid,c);
    printf("Invalid pid syscount: %d\n",getchildsyscount(1000));
    wait(0);
    printf("After wait syscount: %d\n",getchildsyscount(pid));

    return 0;
}