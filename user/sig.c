#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int a = 5;
int b = 7;

void add()
{
    a++;
    b++;
    printf("%d\n", a + b);
    sigreturn();
}

int main()
{
    sigalarm(2, add);
    // printf("\nuser%d", (uint64)&add);
    for (uint64 i = 0; i < 10000000000; i++)
    {
        a += 0;
        if(i == 900000000){
            sigalarm(0,0);
        }
    }
    printf("%d", a + b);
    exit(0);
}