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

typedef struct
{
	const char *arg_Directory;
	int arg_Quality;
	int arg_Workers;
	int arg_Quiet;
	int arg_NoDirs;
} binsort_args_t;

struct BinSort; /* forward declaration */
/*typedef struct BinSort binsort_t;*/

extern binsort_error_t binsort_create(struct BinSort **B,
	binsort_args_t *args);
extern void binsort_destroy(struct BinSort *B);
extern binsort_error_t binsort_run(struct BinSort *B);

#endif /* BINSORT_H */
