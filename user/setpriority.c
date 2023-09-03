#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char **argv)
{
    if(argc != 3){
        printf("Wrong args\n");
        exit(0);
    }
    int new_priority = atoi(argv[1]);
    int pid = atoi(argv[2]);
    set_priority(new_priority, pid);
    exit(0);
}