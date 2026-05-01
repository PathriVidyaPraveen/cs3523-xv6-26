#include "kernel/types.h"
#include "user/user.h"

int main(){

    int pid1 = getpid();
    int pid2 = getpid2();

    printf("getpid() = %d\n",pid1);
    printf("getpid2() = %d\n",pid2);

    return 0;
}
