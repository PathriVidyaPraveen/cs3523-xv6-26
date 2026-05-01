#include "kernel/types.h"
#include "user/user.h"


int main(){

    int n1 = getnumchild();
    printf("Children before fork: %d\n",n1);
    int pid = fork();
    if(pid==0){
        pause(20);
        exit(0);
    }

    pause(5);
    int n2 = getnumchild();
    printf("Children after fork: %d\n",n2);
    wait(0);
    int n3 = getnumchild();
    printf("Children after wait: %d\n",n3);

    return 0;
}