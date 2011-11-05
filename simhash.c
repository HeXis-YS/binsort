
/*
 * Copyright © 2005-2009 Bart Massey
 * ALL RIGHTS RESERVED
 * 
 * [This program is licensed under the "3-clause ('new') BSD License"]
 * 
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the
 * following conditions are met:
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer.
 *     * Redistributions in binary form must reproduce the
 *       above copyright notice, this list of conditions and the
 *       following disclaimer in the documentation and/or other
 *       materials provided with the distribution.
 *     * Neither the name of the copyright holder, nor the names
 *       of other affiliated organizations, nor the names
 *       of other contributors may be used to endorse or promote
 *       products derived from this software without specific prior
 *       written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Generate and compare simple shingleprints
 * Bart Massey 2005/03
 *
 * Code restructured for library use
 * Timm S. Mueller 2011/10
 */

/* Bibliography
 *
 *   Mark Manasse
 *   Microsoft Research Silicon Valley
 *   Finding similar things quickly in large collections
 *   http://research.microsoft.com/research/sv/PageTurner/similarity.htm
 *
 *   Andrei Z. Broder
 *   On the resemblance and containment of documents
 *   In Compression and Complexity of Sequences (SEQUENCES'97),
 *   pages 21-29. IEEE Computer Society, 1998
 *   ftp://ftp.digital.com/pub/DEC/SRC/publications/broder/
 *     positano-final-wpnums.pdf
 *
 *   Andrei Z. Broder
 *   Some applications of Rabin's fingerprinting method
 *   Published in R. Capocelli, A. De Santis, U. Vaccaro eds.
 *   Sequences II: Methods in Communications, Security, and
 *   Computer Science, Springer-Verlag, 1993.
 *   http://athos.rutgers.edu/~muthu/broder.ps
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <arpa/inet.h>
#include "simhash.h"

struct simhash_global
{
	int nshingle;
	int nfeature;
	uint32_t *heap;
	int nheap;
	int maxheap;
	char *occ;
	uint32_t *hash;
	int nhash;
};

/* HASH FILE VERSION */
#define FILE_VERSION 0xcb01

#define NSHINGLE 8
#define NFEATURE 128

/****************************************************************************/

static struct simhash_global *simhash_global_alloc(void)
{
	struct simhash_global *sh = malloc(sizeof *sh);
	if (sh)
	{
		sh->nshingle = NSHINGLE;
		sh->nfeature = NFEATURE;
		
		sh->heap = NULL;
		sh->nheap = 0;
		sh->maxheap = 0;
		
		sh->occ = NULL;
		sh->hash = NULL;
	}
	return sh;
}

static void simhash_global_free(struct simhash_global *sh)
{
	if (sh)
	{
		if (sh->heap)
			free(sh->heap);
		if (sh->occ)
			free(sh->occ);
		if (sh->hash)
			free(sh->hash);
		free(sh);
	}
}

/****************************************************************************/

static const uint32_t crc32_tab[] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

static int hash_crc32(char *buf, int i0, int nbuf)
{
	int i = i0;
	int crc = ~0U;
	do
	{
		crc = crc32_tab[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
		i = (i + 1) % nbuf;
	}
	while (i != i0);
	return crc ^ ~0U;
}

/****************************************************************************/

static int heap_reset(struct simhash_global *sh, int size)
{
	sh->nheap = 0;
	sh->maxheap = size;
	if (sh->heap)
		free(sh->heap);
	sh->heap = malloc(size * sizeof(*sh->heap));
	return (sh->heap != NULL);
}

/* push the top of heap down as needed to
   restore the heap property */
static void downheap(struct simhash_global *sh)
{
	uint32_t *heap = sh->heap;
	int nheap = sh->nheap;
	
	int tmp;
	int i = 0;

	while (1)
	{
		int left = (i << 1) + 1;
		int right = left + 1;

		if (left >= nheap)
			return;
		if (right >= nheap)
		{
			if (heap[i] < heap[left])
			{
				tmp = heap[left];
				heap[left] = heap[i];
				heap[i] = tmp;
			}
			return;
		}
		if (heap[i] >= heap[left] && heap[i] >= heap[right])
			return;
		if (heap[left] > heap[right])
		{
			tmp = heap[left];
			heap[left] = heap[i];
			heap[i] = tmp;
			i = left;
		}
		else
		{
			tmp = heap[right];
			heap[right] = heap[i];
			heap[i] = tmp;
			i = right;
		}
	}
}

static uint32_t heap_extract_max(struct simhash_global *sh)
{
	uint32_t m;
	uint32_t *heap = sh->heap;

	assert(sh->nheap > 0);
	/* lift the last heap element to the top,
	   replacing the current top element */
	m = heap[0];
	heap[0] = heap[--sh->nheap];
	/* now restore the heap property */
	downheap(sh);
	/* and return the former top */
	return m;
}

/* lift the last value on the heap up
   as needed to restore the heap property */
static void upheap(struct simhash_global *sh)
{
	int i = sh->nheap - 1;
	uint32_t *heap = sh->heap;

	assert(sh->nheap > 0);
	while (i > 0)
	{
		int tmp;
		int parent = (i - 1) >> 1;

		if (heap[parent] >= heap[i])
			return;
		tmp = heap[parent];
		heap[parent] = heap[i];
		heap[i] = tmp;
		i = parent;
	}
}

static void heap_insert(struct simhash_global *sh, uint32_t v)
{
	assert(sh->nheap < sh->maxheap);
	sh->heap[sh->nheap++] = v;
	upheap(sh);
}

/****************************************************************************/

/* occupancy is out-of-band.  sigh */
#define EMPTY 0
#define FULL 1
#define DELETED 2

/* for n > 0 */
static int next_pow2(int n)
{
	int m = 1;

	while (n > 0)
	{
		n >>= 1;
		m <<= 1;
	}
	return m;
}

static int hash_alloc(struct simhash_global *sh)
{
	sh->hash = malloc(sh->nhash * sizeof(int));
	sh->occ = malloc(sh->nhash);
	if (sh->hash && sh->occ)
	{
		int i;
		for (i = 0; i < sh->nhash; i++)
			sh->occ[i] = EMPTY;
		return 1;
	}
	return 0;
}

/* The occupancy shouldn't be bad, since we only keep small crcs in
   the stop list */

static int hash_reset(struct simhash_global *sh, int size)
{
	sh->nhash = 7 * size;
	sh->nhash = next_pow2(sh->nhash);
	return hash_alloc(sh);
}

/* Since the input values are crc's, we don't
   try to hash them at all!  they're plenty random
   coming in, in principle. */

static int do_hash_insert(struct simhash_global *sh, uint32_t crc)
{
	int count;
	uint32_t h = crc;
	char *occ = sh->occ;
	uint32_t *hash = sh->hash;
	int nhash = sh->nhash;

	for (count = 0; count < nhash; count++)
	{
		int i = h & (nhash - 1);

		if (occ[i] != FULL)
		{
			occ[i] = FULL;
			hash[i] = crc;
			return 1;
		}
		if (hash[i] == crc)
			return 1;
		h += 2 * (nhash / 4) + 1;
	}
	return 0;
}

/* idiot stop-and-copy for deleted references */
static int gc(struct simhash_global *sh)
{
	int i;
	uint32_t *oldhash = sh->hash;
	char *oldocc = sh->occ;

	if (!hash_alloc(sh))
		return 0;
	
	for (i = 0; i < sh->nhash; i++)
	{
		if (oldocc[i] == FULL)
		{
			if (!do_hash_insert(sh, oldhash[i]))
			{
				fprintf(stderr, "internal error: gc failed, table full\n");
				return 0;
			}
		}
	}
	free(oldhash);
	free(oldocc);
	return 1;
}

static int hash_insert(struct simhash_global *sh, uint32_t crc)
{
	if (do_hash_insert(sh, crc))
		return 1;
	if (!gc(sh))
		return 0;
	if (do_hash_insert(sh, crc))
		return 1;
	fprintf(stderr, "internal error: insert failed, table full\n");
	return 0;
 }

static int do_hash_contains(struct simhash_global *sh, uint32_t crc)
{
	int count;
	uint32_t h = crc;
	int nhash = sh->nhash;
	char *occ = sh->occ;
	uint32_t *hash = sh->hash;

	for (count = 0; count < nhash; count++)
	{
		int i = h & (nhash - 1);

		if (occ[i] == EMPTY)
			return 0;
		if (occ[i] == FULL && hash[i] == crc)
			return 1;
		h += 2 * (nhash / 4) + 1;
	}
	return -1;
}

static int hash_contains(struct simhash_global *sh, uint32_t crc)
{
	int result = do_hash_contains(sh, crc);
	if (result >= 0)
		return result;
	if (!gc(sh))
		abort(); /* TODO: unhandled here */
	result = do_hash_contains(sh, crc);
	if (result >= 0)
		return result;
	/* TODO */
	fprintf(stderr, "internal error: can't find value, table full\n");
	abort();
 }

static int do_hash_delete(struct simhash_global *sh, uint32_t crc)
{
	int count;
	uint32_t h = crc;
	int nhash = sh->nhash;
	char *occ = sh->occ;
	uint32_t *hash = sh->hash;

	for (count = 0; count < nhash; count++)
	{
		int i = h & (nhash - 1);

		if (occ[i] == FULL && hash[i] == crc)
		{
			occ[i] = DELETED;
			return 1;
		}
		if (occ[i] == EMPTY)
			return 0;
		h += 2 * (nhash / 4) + 1;
	}
	return -1;
}

static int hash_delete(struct simhash_global *sh, uint32_t crc)
{
	int result = do_hash_delete(sh, crc);

	if (result >= 0)
		return result;
	if (!gc(sh))
		abort(); /* TODO: unhandled here */
	result = do_hash_delete(sh, crc);
	if (result >= 0)
		return result;
	/* TODO */
	fprintf(stderr, "internal error: delete failed, table full\n");
	abort();
}

/****************************************************************************/

/* if crc is less than top of heap, extract
   top-of-heap, then insert crc.  don't worry
   about sign bits---doesn't matter here. */
static int crc_insert(struct simhash_global *sh, uint32_t crc)
{
	if (sh->nheap == sh->nfeature && crc >= sh->heap[0])
		return 1;
	if (hash_contains(sh, crc))
		return 1;
	if (sh->nheap == sh->nfeature)
	{
		uint32_t m = heap_extract_max(sh);
		int res = hash_delete(sh, m);
		assert(res);
	}
	if (!hash_insert(sh, crc))
		return 0;
	heap_insert(sh, crc);
	return 1;
}

/* return true if the file had enough bytes
   for at least a single shingle. */
static int running_crc(struct simhash_global *sh, char *buf, FILE *f)
{
	int i;
	for (i = 0; i < sh->nshingle; i++)
	{
		int ch = fgetc(f);
		if (ch == EOF)
			return 0;
		buf[i] = ch;
	}
	i = 0;
	while (1)
	{
		int ch;

		if (!crc_insert(sh, (uint32_t) hash_crc32(buf, i, sh->nshingle)))
			return 0; /* error */
		ch = fgetc(f);
		if (ch == EOF)
			break;
		buf[i] = ch;
		i = (i + 1) % sh->nshingle;
	}
	return 1;
}

static struct simhash *alloc_hashinfo(void)
{
	struct simhash *hi = malloc(sizeof *hi);
	if (hi)
		memset(hi, 0, sizeof *hi);
	return hi;
}

static struct simhash *get_hashinfo(struct simhash_global *sh)
{
	struct simhash *hi = alloc_hashinfo();
	if (hi)
	{
		uint32_t *crcs = malloc(sh->nheap * sizeof crcs[0]);
		if (crcs)
		{
			int i = 0;
			hi->nshingle = sh->nshingle;
			hi->nfeature = sh->nheap;
			while (sh->nheap > 0)
				crcs[i++] = heap_extract_max(sh);
			hi->feature = crcs;
			return hi;
		}
		simhash_free(hi);
	}
	return NULL;
}

static struct simhash *hash_file(struct simhash_global *sh, FILE *f)
{
	int success = 0;
	char *buf = malloc(sh->nshingle);
	if (buf)
	{
		if (heap_reset(sh, sh->nfeature))
		{
			if (hash_reset(sh, sh->nfeature))
				success = running_crc(sh, buf, f);
		}
		free(buf);
	}
	if (success)
		return get_hashinfo(sh);
	return NULL;
}

/* walk backward until one set runs out, counting the
   number of elements in the union of the sets.  the
   backward walk is necessary because the common subsets
   are at the end of the file by construction.  bleah.
   should probably reformat so that it's the other way
   around, which would mean that one could shorten a
   shingleprint by truncation. */
static double hashinfo_score(struct simhash *hi1, struct simhash *hi2)
{
	double unionsize;
	double intersectsize;
	int i1 = hi1->nfeature - 1;
	int i2 = hi2->nfeature - 1;
	int count = 0;
	int matchcount = 0;

	while (i1 >= 0 && i2 >= 0)
	{
		if ((uint32_t) (hi1->feature[i1]) < (uint32_t) (hi2->feature[i2]))
		{
			--i1;
			continue;
		}
		if ((uint32_t) (hi1->feature[i1]) > (uint32_t) (hi2->feature[i2]))
		{
			--i2;
			continue;
		}
		matchcount++;
		--i1;
		--i2;
	}
	count = hi1->nfeature;
	if (count > hi2->nfeature)
		count = hi2->nfeature;
	intersectsize = matchcount;
	unionsize = 2 * count - matchcount;
	return intersectsize / unionsize;
}

/****************************************************************************/

void *simhash_free(struct simhash *hi)
{
	if (hi)
	{
		if (hi->feature)
			free(hi->feature);
		free(hi);
	}
	return NULL;
}

struct simhash *simhash_file(FILE *f)
{
	struct simhash *hi = NULL;
	struct simhash_global *sh = simhash_global_alloc();
	if (sh)
	{
		hi = hash_file(sh, f);
		simhash_global_free(sh);
	}
	return hi;
}

#if 0
int simhash_write(struct simhash *hi, FILE *f)
{
	int16_t s = htons(FILE_VERSION);	/* file/CRC version */
	int i, success = fwrite(&s, sizeof(int16_t), 1, f) == 1;
	if (success)
	{
		s = htons(hi->nshingle);
		success = fwrite(&s, sizeof(int16_t), 1, f) == 1;
		for (i = 0; success && i < hi->nfeature; i++)
		{
			uint32_t hv = htonl(hi->feature[i]);
			success = fwrite(&hv, sizeof(uint32_t), 1, f) == 1;
		}
	}
	return success;
}
#endif

const uint8_t *simhash_get(struct simhash *hi, size_t *len)
{
	*len = hi->nfeature * 4;
	return (const uint8_t *) hi->feature;
}

/* fills features with the features from f, and returns a
   pointer to info.  A null pointer is returned on error. */
struct simhash *simhash_read(FILE *f)
{
	struct simhash *h = alloc_hashinfo();
	if (h == NULL)
		return NULL;
	do
	{
		int i = 0;
		uint16_t s, version;
		if (fread(&s, sizeof s, 1, f) != 1)
			break;
		version = ntohs(s);
		if (version != FILE_VERSION)
		{
			fprintf(stderr, "bad file version\n");
			break;
		}
		if (fread(&s, sizeof s, 1, f) != 1)
			break;
		h->nshingle = ntohs(s);
		h->nfeature = 16;
		h->feature = malloc(h->nfeature * sizeof(int));
		if (h->feature == NULL)
			break;
		while (1)
		{
			int fe;
			int nread = fread(&fe, sizeof(int), 1, f);
			if (nread <= 0)
			{
				if (ferror(f))
				{
					perror("fread");
					break;
				}
				h->nfeature = i;
				h->feature = realloc(h->feature, h->nfeature * sizeof(int));
				if (h->feature == NULL)
					break;
				return h;
			}
			if (i >= h->nfeature)
			{
				h->nfeature *= 2;
				h->feature = realloc(h->feature, h->nfeature * sizeof(int));
				if (h->feature == NULL)
					break;
			}
			h->feature[i++] = ntohl(fe);
		}
	} while (0);
	return simhash_free(h);
}

int simhash_compare(struct simhash *h1, struct simhash *h2, double *val)
{
	if (h1 && h2)
	{
		if (h1->nshingle == h2->nshingle)
		{
			*val = hashinfo_score(h1, h2);
			return 1;
		}
	}
	return 0;
}

struct simhash *simhash_create(const char *hash, size_t len)
{
	struct simhash *h = alloc_hashinfo();
	if (h == NULL)
		return NULL;
	h->nshingle = NSHINGLE;
	h->nfeature = len / 4;
	h->feature = malloc(len);
	if (h->feature)
		memcpy(h->feature, hash, len);
	else
	{
		free(h);
		return 0;
	}
	return h;
}

void simhash_init(struct simhash *sh, const uint8_t *hash, size_t len)
{
	sh->nshingle = NSHINGLE;
	sh->nfeature = len / 4;
	sh->feature = (uint32_t *) hash;	
}
