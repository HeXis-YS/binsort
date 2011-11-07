#ifndef BINSORT_H
#define BINSORT_H

/*
**	Binsort - library interface
**
**	Copyright (c) 2011 by Timm S. Mueller <tmueller@schulze-mueller.de>
**	Licensed under the 3-clause BSD license, see COPYRIGHT
**
**	See annotations in README and binsort.c
*/

#include <stdint.h>

#define BINSORT_DEFAULT_QUALITY 15
#define BINSORT_DEFAULT_NUMTHREADS 3

typedef intptr_t binsort_argval_t;

typedef enum 
{
	BINSORT_ARGS_DONE = 0,
	BINSORT_ARGS_USER = 0x40000000,
	BINSORT_ARG_DIRECTORY,
	BINSORT_ARG_QUALITY,
	BINSORT_ARG_NUMWORKERS,
	BINSORT_ARG_QUIET,
	BINSORT_ARG_NODIRS
} binsort_argkey_t;

typedef struct 
{
	binsort_argkey_t key;
	binsort_argval_t value;
} binsort_argitem_t;

typedef enum
{
	BINSORT_ERROR_SUCCESS = 0,
	BINSORT_ERROR_ARGUMENTS,
	BINSORT_ERROR_THREAD_INIT,
	BINSORT_ERROR_THREAD_CREATE,
	BINSORT_ERROR_DIR_OPEN,
	BINSORT_ERROR_DIR_EXAMINE,
	BINSORT_ERROR_OUT_OF_MEMORY,
	BINSORT_ERROR_FILE_OPEN,
	BINSORT_ERROR_HASHING
} binsort_error_t;

struct BinSort; /* forward declaration */
typedef struct BinSort binsort_t;

extern binsort_error_t binsort_create(binsort_t **B, binsort_argitem_t *args);
extern void binsort_destroy(binsort_t *B);
extern binsort_error_t binsort_run(binsort_t *B);

#endif /* BINSORT_H */
