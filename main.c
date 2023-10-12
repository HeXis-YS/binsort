
/*
**	Binsort - main program
**	See annotations in README and binsort.c
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "binsort.h"

#define PROG_NAME "binsort"

typedef struct { const char *key; binsort_argval_t *val, *ptr; } arg_t;

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

enum { ARG_DIR, ARG_QUAL, ARG_NUMT, ARG_QUIET, ARG_NODIR, ARG_HELP1,
	ARG_HELP2, ARG_NUM };

int main(int argc, char **argv)
{
	int res = EXIT_FAILURE;
	arg_t argp[ARG_NUM];
	binsort_argval_t help = 0;
	binsort_argitem_t args[] =
	{
		{ BINSORT_ARG_DIRECTORY, 0 },
		{ BINSORT_ARG_QUALITY, BINSORT_DEFAULT_QUALITY },
		{ BINSORT_ARG_NUMWORKERS, BINSORT_DEFAULT_NUMTHREADS },
		{ BINSORT_ARG_QUIET, 0 },
		{ BINSORT_ARG_NODIRS, 0 },
		{ BINSORT_ARGS_DONE, 0 },
	};
	memset(argp, 0, sizeof argp);
	argp[ARG_DIR].key = "s"; argp[ARG_DIR].ptr = &args[0].value;
	argp[ARG_QUAL].key = "n-o"; argp[ARG_QUAL].ptr = &args[1].value;
	argp[ARG_NUMT].key = "n-t"; argp[ARG_NUMT].ptr = &args[2].value;
	argp[ARG_QUIET].key = "b-q"; argp[ARG_QUIET].ptr = &args[3].value;
	argp[ARG_NODIR].key = "b-d"; argp[ARG_NODIR].ptr = &args[4].value;
	argp[ARG_HELP1].key = "b-h"; argp[ARG_HELP1].ptr = &help;
	argp[ARG_HELP2].key = "b--help"; argp[ARG_HELP2].ptr = &help;
	if (parseargs(argc, argv, argp, ARG_NUM) && !help)
	{
		do
		{
			binsort_t *B;
			binsort_error_t err = BINSORT_ERROR_ARGUMENTS;
			if (!args[ARG_DIR].value)
			{
				printf(PROG_NAME ": Directory argument missing\n");
				break;
			}
			if (args[ARG_QUAL].value < 1 || args[ARG_QUAL].value > 2147483647)
			{
				printf(PROG_NAME 
					": Optimization quality must be between 1 and 2147483647\n");
				break;
			}
			if (args[ARG_NUMT].value < 1 || args[ARG_NUMT].value > 128)
			{
				printf(PROG_NAME 
					": Number of threads must be between 1 and 128\n");
				break;
			}
			err = binsort_create(&B, args);
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
		printf("  -o          Optimization level [1...2147483647], default: %d\n",
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
