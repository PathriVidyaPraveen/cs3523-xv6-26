#include "kernel/types.h"
#include "user/user.h"

int main(){

    printf("Parent getpid() = %d\n",getpid());
    printf("Parent getppid() = %d\n",getppid());

    int pid = fork();
    if(pid == 0){
        printf("Child getpid() = %d\n",getpid());
        printf("Child getppid() = %d\n",getppid());
        return 0;
    }

    wait(0);

    return 0;
}