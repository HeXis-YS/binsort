
/*
**	Binsort - main program
**	See annotations in README and binsort.c
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "binsort.h"

#define PROG_NAME "binsort"
#define BINSORT_DEFAULT_QUALITY 15
#define BINSORT_DEFAULT_NUMTHREADS 3

typedef struct { const char *key; void *val; void *ptr; } arg_t;

static int parseargs(int argc, char **argv, arg_t *args, int numargs)
{
	int i, j, wait = 0, n = -1, k = -1;
	for (i = 1; i < argc; ++i)
	{
		const char *arg = argv[i];
		switch (wait)
		{
			case 0:
				break;
			case 'n':
				args[n].val = args[n].ptr;
				*((int *) args[n].ptr) = atoi(arg);
				wait = 0;
				continue;
		}
		for (j = 0; j < numargs; ++j)
		{
			if (args[j].val)
				continue;
			if (strlen(&args[j].key[1]) > 0)
			{
				if (!strcmp(arg, &args[j].key[1]))
				{
					n = j;
					if (args[j].key[0] == 'n')
						wait = 'n';
					else if (args[j].key[0] == 'b')
						*((int *) args[n].ptr) = 1;
					break;
				}
			}
			else
				k = j;
		}
		if (j == numargs)
		{
			if (k >= 0 && !args[k].val)
			{
				args[k].val = args[k].ptr;
				*((const char **) args[k].ptr) = arg;
				continue;
			}
			return 0;
		}
	}
	if (wait)
		return 0;
	return 1;
}

int main(int argc, char **argv)
{
	int res = EXIT_FAILURE;
	int help = 0;
	binsort_args_t args = 
		{ NULL, BINSORT_DEFAULT_QUALITY, BINSORT_DEFAULT_NUMTHREADS, 0, 0 };
	arg_t argp[7];
	memset(argp, 0, sizeof argp);
	argp[0].key = "s"; argp[0].ptr = &args.arg_Directory;
	argp[1].key = "n-o"; argp[1].ptr = &args.arg_Quality;
	argp[2].key = "n-t"; argp[2].ptr = &args.arg_Workers;
	argp[3].key = "b-q"; argp[3].ptr = &args.arg_Quiet;
	argp[4].key = "b-d"; argp[4].ptr = &args.arg_NoDirs;
	argp[5].key = "b-h"; argp[5].ptr = &help;
	argp[6].key = "b--help"; argp[6].ptr = &help;
	if (parseargs(argc, argv, argp, 7) && !help)
	{
		do
		{
			struct BinSort *B;
			binsort_error_t err = BINSORT_ERROR_ARGUMENTS;
			if (!args.arg_Directory)
			{
				printf(PROG_NAME ": Directory argument missing\n");
				break;
			}
			if (args.arg_Quality < 1 || args.arg_Quality > 1000)
			{
				printf(PROG_NAME 
					": Optimization quality must be between 3 and 128\n");
				break;
			}
			if (args.arg_Workers < 1 || args.arg_Workers > 128)
			{
				printf(PROG_NAME 
					": Number of threads must be between 1 and 128\n");
				break;
			}
			
			err = binsort_create(&B, &args);
			if (!err)
			{
				err = binsort_run(B);
				binsort_destroy(B);
			}
			else
				printf(PROG_NAME 
					": Error number %d occurred, cannot continue\n", err);
			if (!err)
				err = EXIT_SUCCESS;
		} while (0);
	}
	else
	{
		printf("Usage: %s [options] dir\n", PROG_NAME);
		printf("Options:\n");
		printf("  -o          Optimization level [1...1000], default: %d\n",
			BINSORT_DEFAULT_QUALITY);
		printf("  -t          Number of threads [1...128], default: %d\n",
			BINSORT_DEFAULT_NUMTHREADS);
		printf("  -q          Quiet operation, no progress indicators\n");
		printf("  -d          Do not include directories in output list\n");
		printf("  -h  --help  This help\n");
		printf("\nNote: Results are not stable unless you specify -t 1.\n");
	}
	
	return res;
}
