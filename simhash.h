#ifndef SIMHASH_H
#define SIMHASH_H

/*
 * Simhash shingleprinting library
 *
 * Original version:
 * Copyright © 2005-2009 Bart Massey
 * 
 * Library version:
 * Copyright © 2011 by Timm S. Mueller
 * 
 * See the Copyright notice and bibliography in simh.c
 * And the general copyright notice in COPYRIGHT
 */

#include <stdio.h>
#include <stdint.h>

struct simhash
{
	uint32_t *feature;
	int32_t nfeature;
	uint16_t nshingle;
};

extern void *simhash_free(struct simhash *hi);
extern struct simhash *simhash_file(FILE *f);
extern const uint8_t *simhash_get(struct simhash *hi, size_t *len);
extern struct simhash *simhash_read(FILE *f);
extern int simhash_compare(struct simhash *h1, struct simhash *h2, double *val);
extern void simhash_init(struct simhash *sh, const uint8_t *hash, size_t len);
/* extern void simhash_write(struct simhash *hi, FILE *f); */

#endif
