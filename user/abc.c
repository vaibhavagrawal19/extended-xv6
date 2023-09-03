#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int main(){
    int pid = fork();
    if(pid == 0){
        settickets(100);
        for(int i = 0;i< 100000000000;i++){
            ;
        }

        // printf("end 4\n\n\n\n");
    }
    else{
        for(int i = 0;i< 100000000000;i++){
            ;
        }
        // printf("end 3\n\n\n");
    }
}