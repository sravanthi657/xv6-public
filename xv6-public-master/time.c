#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"

int main(int argc, char **argv)
{
	int wtime, rtime, status = 0;
	int pid = fork(),flag=0;
	if (argc == 1)flag=1;
	if (pid == 0)
	{
		if (flag)
		{
			printf(1, "No argument is passed , So Timing the deafult one\n");
			exit();
		}

		else
		{
			printf(1, "Timing %s\n", argv[1]);
			if (exec(argv[1], argv + 1) < 0)
			{
				printf(2, "Exec %s failed\n", argv[1]);
				exit();
			}
		}
	}

	else if (pid > 0)
	{
		status = waitx(&wtime, &rtime);
		if (flag)
		{
			printf(1, "Time taken by default process\nWait time: %d Run time %d with Status %d\n\n", wtime, rtime, status);
		}

		else
			printf(1, "Wait time %d Run time %d with Status %d by %s\n\n", wtime, rtime, status,argv[1]);
		exit();
	}
}
