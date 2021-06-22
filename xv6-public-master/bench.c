#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "fs.h"
#define MAX 100000000
int main(int argc, char *argv[])
{

    
    for(int i=0;i<10;i++)
    {
        int pid=fork();
        if(pid < 0)
        {
            printf(1,"Fork failed\n");
        }
        if(pid==0)
        {
            volatile int i;
            for (volatile int k = 0; k < 10; k++)
            {
                if(k>i)
                {
                    for (i = 0; i <MAX; i++)
                    {
                        ; //cpu time
                    }
                }
                else
                {
                    sleep(200); //io time
                }
            }
            exit();    
        }

    }
for (int i = 0; i < 15; i++) wait();

    exit();
}