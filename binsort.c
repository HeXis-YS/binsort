
/*
**	Binsort - sort files by binary similarity
**
**	Copyright (c) 2011 by Timm S. Mueller <tmueller@schulze-mueller.de>
**	Licensed under the 3-clause BSD license, see COPYRIGHT
**
**	Scans the contents of a directory, groups the files by binary
**	similarity, generates a filelist and prints the list to stdout. One
**	possible application is to pass the list to an archiving tool, e.g.:
**
**	$ binsort <dir> | tar -T- --no-recursion -czf out.tar.gz
**
**	This can improve compression rates considerably, although sorting is
**	in no way optimized for a particular compression algorithm.
**
**	This is a research project combining threshold accepting,
**	shingleprinting, and massive multithreading. It uses simhash by Bart
**	Massey [1], and Tiny Mersenne Twister by Mutsuo Saito and Makoto
**	Matsumoto [2]. See COPYRIGHT for the respective copyright holders'
**	licensing terms.
**
**	References and further reading:
**
**	[1] http://svcs.cs.pdx.edu/gitweb/simhash.git
**	[2] http://www.math.sci.hiroshima-u.ac.jp/~m-mat/MT/TINYMT/index.html
**	See also bibliography in simhash.c
*/

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "xpthread.h"
#include "simhash.h"
#include "tinymt32.h"
#include "binsort.h"

typedef int64_t num_t;
typedef int64_t dist_t;
typedef int bool_t;

typedef enum 
{
	MSG_HASH,
	MSG_CALCDIST,
	MSG_OPTIMIZE
} msgtype_t;

struct Node
{
	struct Node *ln_Succ;
	struct Node *ln_Pred;
};

struct Message
{
	struct XPMessage msg_XPMessage;
	void *msg_Data;
	msgtype_t msg_Type;
};

struct List
{
	struct Node *lh_Head;
	struct Node *lh_Tail;
	struct Node *lh_TailPred;
};

struct HashNode
{
	struct Node hn_Node;
	size_t hn_Size;
	uint8_t *hn_Hash;
};

struct HashMessage
{
	struct Message hm_Message;
	struct HashNode *hm_StartNode;
	num_t hm_FirstIndex;
	num_t hm_LastIndex;
};

struct Random
{
	uint64_t rnd_RandPool;
	tinymt32_t rnd_Seed;
	num_t rnd_NumRange;
	num_t rnd_BitMask;
	num_t rnd_NumBits;
	num_t rnd_RandBits;
};

struct OptMessage
{
	struct Message om_Message;
	struct Random om_Random;
	num_t om_NumIterations;
};

struct DirList
{
	struct List dls_Head;
	num_t dls_NumTotal;
	num_t dls_NumFiles;
};

struct DirEntry
{
	struct Node den_Node;
	struct Message den_Message;
	const char *den_Name;
	struct HashNode *den_HashNode;
	num_t den_Index;
	binsort_error_t den_Error;
	bool_t den_IsFile;
	bool_t den_IsDir;
};

struct RangeNode 
{
	struct Node rn_Node;
	num_t rn_First, rn_Last;
};

struct BinSort
{
	/* List of all filesystem objects in directory */
	struct DirList b_DirList;
	/* List of simhashes */
	struct List b_Hashes;
	/* Distances array */
	uint8_t *b_Distances;
	/* Threading library base */
	struct XPBase *b_XPBase;
	/* Main thread context */
	struct XPThread *b_Self;
	/* Array of worker threads */
	struct XPThread **b_Workers;
	/* Message port for worker replies */
	struct XPPort *b_ReplyPort;
	/* Directory name */
	const char *b_Directory;
	/* Number of files */
	num_t b_NumFiles;
	/* Number of hashes */
	num_t b_NumHashes;
	/* Number of distances left to calculate */
	num_t b_DistancesLeft;
	/* Distance of initial order */
	dist_t b_InitialDistance;
	/* Optimization quality */
	int b_Quality;
	/* Number of worker threads */
	int b_NumWorkers;
	/* Silent operation */
	bool_t b_Quiet;
	/* No directories in output */
	bool_t b_NoDirs;
	/* Signal bit for worker replyport */
	XPSIGMASK b_ReplyPortSignal;
	/* Locking object for the following fields */
	struct XPFastMutex *b_Lock;
	/* Current order of file entries */
	struct DirEntry **b_Order;
	/* List of ranges currently being processed by workers */
	struct List b_RangeList;
	/* Sum of current order's file distances */
	dist_t b_CurrentDistance;
};

#define XPT_SIG_UPDATE	0x00010000

static void binsort_freedir(binsort_t *B);



/*
**	tag = GetTag(argitemlist, key, defvalue)
**	Get argument item value
*/

static binsort_argval_t GetTag(binsort_argitem_t *taglist,
	binsort_argkey_t tag, binsort_argval_t defvalue)
{
	while (taglist)
	{
		binsort_argkey_t listtag = taglist->key;
		switch (listtag)
		{
			case BINSORT_ARGS_DONE:
				return defvalue;
			default:
				if (tag == listtag)
					return taglist->value;
				taglist++;
		}
	}
	return defvalue;
}


/*
**	InitList(list)
**	Prepare list header
*/

static void InitList(struct List *list)
{
	list->lh_TailPred = (struct Node *) list;
	list->lh_Tail = NULL;
	list->lh_Head = (struct Node *) &list->lh_Tail;
}

/*
**	AddTail(list, node)
**	Add a node at the tail of a list
*/

static __inline void AddTail(struct List *list, struct Node *node)
{
	struct Node *temp = list->lh_TailPred;
	list->lh_TailPred = node;
	node->ln_Succ = (struct Node *) &list->lh_Tail;
	node->ln_Pred = temp;
	temp->ln_Succ = node;
}

/*
**	node = RemTail(list)
**	Unlink and return a list's last node
*/

static struct Node *RemTail(struct List *list)
{
	struct Node *temp = list->lh_TailPred;
	if (temp->ln_Pred)
	{
		list->lh_TailPred = temp->ln_Pred;
		temp->ln_Pred->ln_Succ = (struct Node *) &list->lh_Tail;
		return temp;
	}
	return NULL;
}

/*
**	Remove(node)
**	Unlink node from a list
*/

static __inline void Remove(struct Node *node)
{
	struct Node *temp = node->ln_Succ;
	node->ln_Pred->ln_Succ = temp;
	temp->ln_Pred = node->ln_Pred;
}


/*
**	err = dirlist_scan(dirlist, dirname)
**	Scan directory recursively. Note that this function would not be
**	thread-safe in a library, due to its use of readdir() and strerror()
*/

static binsort_error_t dirlist_scan(struct DirList *list, const char *dirname)
{
	struct Node *next, *node = list->dls_Head.lh_TailPred;
	binsort_error_t err = BINSORT_ERROR_SUCCESS;
	int res = 0;
	num_t num = 0;
	struct dirent *dp;
	size_t pathlen = strlen(dirname);
	DIR *dir = opendir(dirname);
	if (dir == NULL)
	{
		fprintf(stderr, "%s : %s\n", dirname, strerror(errno));
		return BINSORT_ERROR_DIR_OPEN;
	}
	if (pathlen > 0 && dirname[pathlen - 1] == '/')
		pathlen--;
	while ((errno = 0, dp = readdir(dir)))
	{
		struct DirEntry *direntry;
		size_t nlen;
		const char *name = dp->d_name;
		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;
		nlen = strlen(name);
		direntry = malloc(sizeof *direntry + pathlen + nlen + 2);
		if (direntry)
		{
			struct stat statbuf;
			char *p = (char *) (direntry + 1);
			memset(direntry, 0, sizeof *direntry);
			direntry->den_Message.msg_Data = direntry; /* backptr */
			direntry->den_Index = -1;
			strcpy(p, dirname);
			p[pathlen] = '/';
			strcpy(p + pathlen + 1, name);
			if (stat(p, &statbuf) == 0)
			{
				direntry->den_Name = p;
				direntry->den_IsFile = S_ISREG(statbuf.st_mode);
				direntry->den_IsDir = S_ISDIR(statbuf.st_mode);
				AddTail(&list->dls_Head, &direntry->den_Node);
				list->dls_NumTotal++;
				if (direntry->den_IsFile)
					list->dls_NumFiles++;
				num++;
			}
			else
				fprintf(stderr, "%s : %s\n", p, strerror(errno));
		}
	}
	res = errno;
	closedir(dir);
	if (res != 0)
	{
		fprintf(stderr, "%s : %s\n", dirname, strerror(res));
		err = BINSORT_ERROR_DIR_EXAMINE;
	}
	if (!err)
	{
		num_t i = 0;
		node = node->ln_Succ;
		for (; i < num && (next = node->ln_Succ); node = next, ++i)
		{
			struct DirEntry *dn = (struct DirEntry *) node;
			if (dn->den_IsDir)
				dirlist_scan(list, dn->den_Name);
		}
	}
	return err;
}

/*
**	err = binsort_genhashes(binsort)
**	Generate hashes for all files
*/

static binsort_error_t binsort_genhashes(binsort_t *B)
{
	struct XPBase *xpbase = B->b_XPBase;
	struct XPPort *rport = B->b_ReplyPort;
	XPSIGMASK sig, portsig = B->b_ReplyPortSignal;
	struct Node *next, *node = B->b_DirList.dls_Head.lh_Head;
	int numworkers = B->b_NumWorkers;
	num_t sent = 0;
	int quiet = B->b_Quiet;
	
	for (; (next = node->ln_Succ); node = next)
	{
		struct DirEntry *direntry = (struct DirEntry *) node;
		if (direntry->den_IsFile)
		{
			struct XPThread *worker = B->b_Workers[sent % numworkers];
			struct XPPort *port = (*xpbase->getuserport)(xpbase, worker);
			direntry->den_Message.msg_Type = MSG_HASH;
			(*xpbase->putmsg)(xpbase, port, rport, 
				&direntry->den_Message.msg_XPMessage);
			sent++;
		}
	}
	
	while (sent > 0)
	{
		struct XPMessage *msg;
		sig = (*xpbase->wait)(xpbase, portsig);
		while ((msg = (*xpbase->getmsg)(xpbase, rport)))
		{
			struct DirEntry *direntry = ((struct Message *) msg)->msg_Data;
			struct HashNode *hashnode = direntry->den_HashNode;
			if (hashnode)
			{
				direntry->den_Index = B->b_NumHashes++;
				AddTail(&B->b_Hashes, &hashnode->hn_Node);
			}
			if ((--sent & 127) == 0 && !quiet)
				fprintf(stderr, "%ld files left   \r", (long int) sent);
		}
	}
	
	return BINSORT_ERROR_SUCCESS;
}

/*
**	err = binsort_gendistances(binsort)
**	Calculate distance array
*/

binsort_error_t binsort_gendistances(binsort_t *B)
{
	struct XPBase *xpbase = B->b_XPBase;
	struct XPPort *rport = B->b_ReplyPort;
	XPSIGMASK portsig = B->b_ReplyPortSignal;
	int numworkers = B->b_NumWorkers;
	struct List *hashes = &B->b_Hashes;
	num_t num = B->b_NumHashes;
	struct HashMessage *msgs = malloc(sizeof *msgs * numworkers);
	if (!msgs)
		return BINSORT_ERROR_OUT_OF_MEMORY;
	
	/* avoid overproportional number of workers per distances: */
	if (num / 10 < numworkers)
	{
		numworkers = num / 10;
		if (numworkers < 1)
			numworkers = 1;
	}
	
	do
	{
		num_t y, i, i0;
		struct Node *ynext, *ynode = hashes->lh_Head;
		double a0 = num * num / numworkers;
		uint8_t *d = B->b_Distances = malloc(num * num);
		if (d == NULL)
			break;
		B->b_DistancesLeft = (num - 1) * (num - 1) / 2;
		for (i = i0 = y = 0; (ynext = ynode->ln_Succ); ynode = ynext, ++y)
		{
			if (y == i0)
			{
				struct HashNode *yhash = (struct HashNode *) ynode;
				struct XPThread *worker = B->b_Workers[i];
				struct XPPort *port = (*xpbase->getuserport)(xpbase, worker);
				num_t i1 = num - 1;
				if (i < numworkers - 1)
					i1 = floor(sqrt((i + 1) * a0)) - 1;
				msgs[i].hm_Message.msg_Type = MSG_CALCDIST;
				msgs[i].hm_Message.msg_Data = &msgs[i];
				msgs[i].hm_StartNode = yhash;
				msgs[i].hm_FirstIndex = i0;
				msgs[i].hm_LastIndex = i1;
				(*xpbase->putmsg)(xpbase, port, rport, 
					&msgs[i].hm_Message.msg_XPMessage);
				i0 = i1 + 1;
				i++;
			}
		}
		B->b_Distances = d;
	} while (0);
	do
	{
		XPSIGMASK sig = (*xpbase->wait)(xpbase, portsig | XPT_SIG_UPDATE);
		while ((*xpbase->getmsg)(xpbase, rport))
			--numworkers;
		if (!B->b_Quiet && (sig & XPT_SIG_UPDATE))
		{
			fprintf(stderr, "%ld distances left           \r", 
				(long int) B->b_DistancesLeft);
		}
	} while (numworkers > 0);
	free(msgs);
	return BINSORT_ERROR_SUCCESS;
}

/*
**	get distance (and starting position)
*/

static dist_t getdist(binsort_t *B, num_t *p_startpos)
{
	struct DirEntry **order = B->b_Order;
	num_t num = B->b_NumFiles;
	uint8_t *array = B->b_Distances;
	num_t arrnum = B->b_NumHashes;
	dist_t d = 0;
	num_t i0;
	num_t startpos = 0;
	dist_t worstd = -1;
	for (i0 = 0; i0 < num; ++i0)
	{
		num_t a = order[i0]->den_Index;
		num_t b = order[(i0 + 1) % num]->den_Index;
		if (a >= 0 && b >= 0)
		{
			dist_t dd = array[a * arrnum + b];
			d += dd;
			if (dd > worstd)
			{
				worstd = dd;
				startpos = i0;
			}
		}
	}
	if (p_startpos)
		*p_startpos = startpos;
	return d;
}

/*
**	random
*/

static void random_init(struct Random *rnd, num_t range)
{
	num_t bitmask = 1;
	num_t numbits = 1;
	while (bitmask < range)
	{
		bitmask = (bitmask << 1) | 1;
		numbits++;
	}
	rnd->rnd_RandPool = 0;
	rnd->rnd_NumRange = range;
	rnd->rnd_BitMask = bitmask;
	rnd->rnd_NumBits = numbits;
	rnd->rnd_RandBits = 0; /* number of bits in pool */
}

static uint32_t random_get(struct Random *rnd)
{
	uint64_t randpool = rnd->rnd_RandPool;
	num_t randbits = rnd->rnd_RandBits;
	num_t numbits = rnd->rnd_NumBits;
	uint32_t r;
	do
	{
		if (randbits < 32)
		{
			randpool |= 
				((uint64_t) tinymt32_generate_uint32(&rnd->rnd_Seed)) 
					<< randbits;
			randbits += 32;
		}
		r = randpool & rnd->rnd_BitMask;
		randpool >>= numbits;
		randbits -= numbits;
	} while (r >= rnd->rnd_NumRange);
	rnd->rnd_RandPool = randpool;
	rnd->rnd_RandBits = randbits;
	return r;
}

/*
**	generate order - optimization main function
*/

static binsort_error_t binsort_genorder(binsort_t *B)
{
	struct XPBase *xpbase = B->b_XPBase;
	struct XPPort *rport = B->b_ReplyPort;
	XPSIGMASK portsig = B->b_ReplyPortSignal;
	struct DirList *dlist = &B->b_DirList;
	struct Node *next, *node;
	struct DirEntry **order;
	int numworkers = B->b_NumWorkers;
	int quality = B->b_Quality;
	num_t startpos, num;
	dist_t d;
	int i;
	struct OptMessage *msgs = malloc(sizeof *msgs * numworkers);
	if (!msgs)
		return BINSORT_ERROR_OUT_OF_MEMORY;
	
	B->b_Order = order = malloc(sizeof *order * dlist->dls_NumFiles);
	if (order == NULL)
		return BINSORT_ERROR_OUT_OF_MEMORY;

	/* remove files from list, add to array: */
	node = dlist->dls_Head.lh_Head;
	for (num = 0; (next = node->ln_Succ); node = next)
	{
		struct DirEntry *entry = (struct DirEntry *) node;
		if (entry->den_IsFile)
		{
			Remove(node);
			order[num++] = entry;
		}
	}
	
	B->b_NumFiles = num;

	/* distribute optimization to workers: */
	d = getdist(B, NULL);
	/*fprintf(stderr, "initial distance: %ld - files: %ld - hashes: %ld\n", 
		(long int) d, (long int) B->b_NumFiles, B->b_NumHashes);*/
	B->b_CurrentDistance = d;
	B->b_InitialDistance = d;
	
	for (i = 0; i < numworkers; ++i)
	{
		struct XPThread *worker = B->b_Workers[i];
		struct XPPort *port = (*xpbase->getuserport)(xpbase, worker);
		msgs[i].om_Message.msg_Type = MSG_OPTIMIZE;
		msgs[i].om_Message.msg_Data = &msgs[i];
		memset(&msgs[i].om_Random.rnd_Seed, 0x5a, 
			sizeof msgs[i].om_Random.rnd_Seed);
		tinymt32_init(&msgs[i].om_Random.rnd_Seed, 4567 + i);
		random_init(&msgs[i].om_Random, num);
		msgs[i].om_NumIterations = pow(d, 1.1) * quality / numworkers;
		(*xpbase->putmsg)(xpbase, port, rport, 
			&msgs[i].om_Message.msg_XPMessage);
	}
	do
	{
		XPSIGMASK sig = (*xpbase->wait)(xpbase, portsig | XPT_SIG_UPDATE);
		while ((*xpbase->getmsg)(xpbase, rport))
			--numworkers;
		if (!B->b_Quiet && (sig & XPT_SIG_UPDATE))
			fprintf(stderr, "d=%ld         \r", 
				(long int) B->b_CurrentDistance);
	} while (numworkers > 0);

	d = getdist(B, &startpos);
	assert(B->b_CurrentDistance == d);

	if (B->b_NoDirs)
		binsort_freedir(B);

	/* add files back to list: */
	for (i = 0; i < num; ++i)
		AddTail(&dlist->dls_Head, &order[(i + startpos + 1) % num]->den_Node);

	free(msgs);
	return BINSORT_ERROR_SUCCESS;
}

/*
**	optimization worker
*/

static __inline dist_t getdelta(struct DirEntry **order, const uint8_t *array, 
	num_t array_num, num_t i0, num_t i1, num_t i00, num_t i11)
{
	dist_t delta = 0;

	num_t a = order[i0]->den_Index;
	num_t b = order[i1]->den_Index;
	num_t c = order[i00]->den_Index;
	num_t d = order[i11]->den_Index;
	
	if (a >= 0)
	{
		a *= array_num;
		if (c >= 0) delta -= array[a + c];
		if (d >= 0) delta += array[a + d];
	}

	if (b >= 0)
	{
		b *= array_num;
		if (d >= 0) delta -= array[b + d];
		if (c >= 0) delta += array[b + c];
	}
	
	return delta;
}

static void binsort_worker_optimize(struct XPBase *xpbase, binsort_t *B,
	struct OptMessage *msg)
{
	struct Random *random = &msg->om_Random;
	struct DirEntry **order = B->b_Order;
	num_t num = B->b_NumFiles;
	struct List *rangelist = &B->b_RangeList;
	struct XPFastMutex *lock = B->b_Lock;
	dist_t delta;
	struct Node *node, *next;
	num_t i0, i1, n, i;
	num_t i11, i00;
	num_t arrnum = B->b_NumHashes;
	const uint8_t *array = B->b_Distances;
	dist_t inidist = B->b_InitialDistance * 20;
	double dunk = (double) inidist * 1.1 / msg->om_NumIterations;

	for (i = 1; i < msg->om_NumIterations; ++i)
	{
		double thresh = (double) inidist / i - dunk;
		if (thresh < 0) thresh = 0;

		if ((i & 65535) == 0)
			(*xpbase->signal)(xpbase, B->b_Self, XPT_SIG_UPDATE);

		again:
		
		i0 = random_get(random);
		i1 = random_get(random);
		
		n = 0;

		if (i0 >= i1 + 2)
		{
			n = num - i0 + i1 + 1;
			if (n > i0 - i1 - 1)
			{
				num_t t = i1 + 1;
				i1 = i0 - 1;
				i0 = t;
				n = i1 - i0 + 1;
			}
		}
		else if (i1 > i0 && i1 - i0 <= num - 2)
		{
			if (num - i1 + i0 - 1 < i1 - i0 + 1)
			{
				num_t t = i0;
				if (--t < 0) t += num;
				i0 = (i1 + 1) % num;
				i1 = t;
				if (i0 > i1)
					n = num - i0 + i1 + 1;
				else
					n = i1 - i0 + 1;
			}
			else
				n = i1 - i0 + 1;
		}
		else
			goto again;

		
		delta = 0;
		i11 = (i1 + 1) % num;
		i00 = i0 - 1;
		if (i00 < 0) i00 += num;
		
		(*xpbase->lockfastmutex)(xpbase, lock);
		
		node = rangelist->lh_Head;
		for (; (next = node->ln_Succ); node = next)
		{
			struct RangeNode *rn = (struct RangeNode *) node;
			num_t a = rn->rn_First;
			num_t b = rn->rn_Last;
			if (a < b)
			{
				if (i0 < i1)
				{
					if (i1 < a || i0 > b) 
						continue;
				}
				else
				{
					if (i1 < a && i0 > b)
						continue;
				}
			}
			else
			{
				if (i0 < i1 && i0 > b && i1 > b && i1 < a && i0 < a) 
					continue;
			}

			(*xpbase->unlockfastmutex)(xpbase, lock);
			goto again;
		}
		
		delta = getdelta(order, array, arrnum, i0, i1, i00, i11);
		if (delta < thresh)
		{
			struct RangeNode rangelock;
			struct DirEntry *t;
			num_t i;

			B->b_CurrentDistance += delta;

			t = order[i0];
			order[i0] = order[i1];
			order[i1] = t;

			if (n > 3)
			{
				rangelock.rn_First = i0;
				rangelock.rn_Last = i1;
				AddTail(rangelist, &rangelock.rn_Node);
				(*xpbase->unlockfastmutex)(xpbase, lock);

				i0 = (i0 + 1) % num;
				if (--i1 < 0) i1 += num;
				
				for (i = 1; i < n / 2; ++i)
				{
					t = order[i0];
					order[i0] = order[i1];
					order[i1] = t;
					i0 = (i0 + 1) % num;
					if (--i1 < 0) i1 += num;
				}

				(*xpbase->lockfastmutex)(xpbase, lock);
				Remove(&rangelock.rn_Node);
			}
		}
		
		(*xpbase->unlockfastmutex)(xpbase, lock);
	}
}


/*
**	simhash worker
*/

static void binsort_worker_hash(struct Message *msg)
{
	binsort_error_t err = BINSORT_ERROR_FILE_OPEN;
	struct DirEntry *direntry = msg->msg_Data;
	FILE *f = fopen(direntry->den_Name, "rb");
	if (f)
	{
		struct simhash *h;
		err = BINSORT_ERROR_HASHING;
		h = simhash_file(f);
		fclose(f);
		if (h)
		{
			size_t hsize;
			const uint8_t *hash = simhash_get(h, &hsize);
			struct HashNode *hn = malloc(sizeof *hn + hsize);
			err = BINSORT_ERROR_OUT_OF_MEMORY;
			if (hn)
			{
				hn->hn_Hash = (uint8_t *) (hn + 1);
				memcpy(hn->hn_Hash, hash, hsize);
				hn->hn_Size = hsize;
				err = BINSORT_ERROR_SUCCESS;
				direntry->den_HashNode = hn;
			}
			simhash_free(h);
		}
		/*else
			fprintf(stderr, "error hashing %s\n", direntry->den_Name);*/
	}
	direntry->den_Error = err;
}


/*
**	distance calculation worker
*/

static void binsort_worker_calcdist(struct XPBase *xpbase, binsort_t *B,
	struct HashMessage *msg)
{
	num_t y, x, i = 0;
	struct Node *ynext, *ynode = (struct Node *) msg->hm_StartNode;
	struct List *hashes = &B->b_Hashes;
	uint8_t *array = B->b_Distances;
	num_t num = B->b_NumHashes;
	for (y = msg->hm_FirstIndex; 
		y <= msg->hm_LastIndex && (ynext = ynode->ln_Succ); 
		ynode = ynext, ++y)
	{
		struct HashNode *yhash = (struct HashNode *) ynode;
		struct Node *xnext, *xnode = hashes->lh_Head;
		struct simhash h1, h2;
		simhash_init(&h1, yhash->hn_Hash, yhash->hn_Size);
		for (x = 0; x < y && (xnext = xnode->ln_Succ); xnode = xnext, ++x)
		{
			struct HashNode *xhash = (struct HashNode *) xnode;
			double val;
			int vali;
			simhash_init(&h2, xhash->hn_Hash, xhash->hn_Size);
			simhash_compare(&h1, &h2, &val);
			vali = val * 255;
			vali = 255 - vali;
			array[x + y * num] = vali;
			array[y + x * num] = vali;
			if ((++i & 262143) == 0)
			{
				(*xpbase->lockfastmutex)(xpbase, B->b_Lock);
				B->b_DistancesLeft -= 262144;
				(*xpbase->unlockfastmutex)(xpbase, B->b_Lock);
				(*xpbase->signal)(xpbase, B->b_Self, XPT_SIG_UPDATE);
			}
		}
	}
}


/*
**	worker thread entry
*/

static void binsort_worker(struct XPBase *xpbase)
{
	struct XPThread *self = (*xpbase->findthread)(xpbase, NULL);
	struct XPPort *port = (*xpbase->getuserport)(xpbase, self);
	XPSIGMASK sig, portsig = (*xpbase->getportsignal)(xpbase, port);
	binsort_t *B = (*xpbase->getdata)(xpbase, self);
	do
	{
		struct XPMessage *xpmsg;
		sig = (*xpbase->wait)(xpbase, portsig | XPT_SIG_ABORT);
		while ((xpmsg = (*xpbase->getmsg)(xpbase, port)))
		{
			struct Message *msg = (struct Message *) xpmsg;
			switch (msg->msg_Type)
			{
				case MSG_HASH:
					binsort_worker_hash(msg);
					break;
				case MSG_CALCDIST:
					binsort_worker_calcdist(xpbase, B,
						(struct HashMessage *) msg);
					break;
				case MSG_OPTIMIZE:
					binsort_worker_optimize(xpbase, B,
						(struct OptMessage *) msg);
					break;
			}
			(*xpbase->replymsg)(xpbase, xpmsg);
		}
	} while (!(sig & XPT_SIG_ABORT));
}


/*
**	init, free
*/

static void binsort_freeworkers(binsort_t *B)
{
	if (B->b_Workers)
	{
		int i, nt = B->b_NumWorkers;
		for (i = 0; i < nt; ++i)
		{
			struct XPThread *thread = B->b_Workers[i];
			if (thread)
				(*B->b_XPBase->signal)(B->b_XPBase, thread, XPT_SIG_ABORT);
		}
		for (i = 0; i < nt; ++i)
		{
			struct XPThread *thread = B->b_Workers[i];
			if (thread)
				(*B->b_XPBase->destroythread)(B->b_XPBase, thread);
		}
		free(B->b_Workers);
		B->b_Workers = NULL;
	}
}

static binsort_error_t binsort_initworkers(binsort_t *B)
{
	int i, nt = B->b_NumWorkers;
	B->b_Workers = malloc(sizeof *B->b_Workers * nt);
	for (i = 0; i < nt; ++i)
	{
		B->b_Workers[i] = (*B->b_XPBase->createthread)(B->b_XPBase,
			binsort_worker, B, NULL);
		if (B->b_Workers[i] == NULL)
		{
			binsort_freeworkers(B);
			return BINSORT_ERROR_THREAD_CREATE;
		}
	}
	return BINSORT_ERROR_SUCCESS;
}

static binsort_error_t binsort_init(binsort_t *B, binsort_argitem_t *args)
{
	binsort_error_t err = BINSORT_ERROR_ARGUMENTS;
	memset(B, 0, sizeof *B);
	InitList(&B->b_DirList.dls_Head);
	InitList(&B->b_Hashes);
	InitList(&B->b_RangeList);
	while (args)
	{
		struct XPBase *xpbase;

		B->b_Directory = (const char *) GetTag(args,
			BINSORT_ARG_DIRECTORY, 0);
		if (B->b_Directory == NULL)
			break;
		B->b_Quality = GetTag(args, 
			BINSORT_ARG_QUALITY, BINSORT_DEFAULT_QUALITY);
		if (B->b_Quality < 1 || B->b_Quality > 2147483647)
			break;
		B->b_NumWorkers = GetTag(args,
			BINSORT_ARG_NUMWORKERS, BINSORT_DEFAULT_NUMTHREADS);
		if (B->b_NumWorkers < 1 || B->b_NumWorkers > 128)
			break;
		B->b_Quiet = GetTag(args, BINSORT_ARG_QUIET, 0);
		B->b_NoDirs = GetTag(args, BINSORT_ARG_NODIRS, 0);
		B->b_NumWorkers = GetTag(args, BINSORT_ARG_NUMWORKERS, 0);
		
		err = BINSORT_ERROR_THREAD_INIT;
		B->b_XPBase = xpbase = xpthread_create(NULL);
		if (xpbase == NULL)
			break;
		B->b_Lock = (*xpbase->createfastmutex)(xpbase);
		if (B->b_Lock == NULL)
			break;
		B->b_Self = (*xpbase->findthread)(xpbase, NULL);
		B->b_ReplyPort = (*xpbase->getuserport)(xpbase, B->b_Self);
		B->b_ReplyPortSignal = (*xpbase->getportsignal)(xpbase, 
			B->b_ReplyPort);
		return binsort_initworkers(B);
	}
	return err;
}

static void binsort_freedir(binsort_t *B)
{
	struct Node *node;
	while ((node = RemTail(&B->b_DirList.dls_Head)))
		free(node);
}

static void binsort_freehashes(binsort_t *B)
{
	struct Node *node;
	while ((node = RemTail(&B->b_Hashes)))
		free(node);
}

static void binsort_freedistances(binsort_t *B)
{
	if (B->b_Distances != NULL)
		free(B->b_Distances);
	B->b_Distances = NULL;
}

static void binsort_free(binsort_t *B)
{
	if (B->b_Order != NULL)
		free(B->b_Order);
	
	binsort_freedistances(B);
	binsort_freehashes(B);
	binsort_freedir(B);
	
	binsort_freeworkers(B);
	
	if (B->b_Lock)
		(*B->b_XPBase->destroyfastmutex)(B->b_XPBase, B->b_Lock);
	
	if (B->b_XPBase != NULL)
		xpthread_destroy(B->b_XPBase);
}

/*
**	Public interface starts here
*/

/*
**	err = binsort_run(binsort, dirname)
**	binsort main procedure
*/

binsort_error_t binsort_run(binsort_t *B)
{
	binsort_error_t err;
	bool_t quiet = B->b_Quiet;
	do
	{
		err = dirlist_scan(&B->b_DirList, B->b_Directory);
		if (err)
		{
			fprintf(stderr, "*** error scanning directory\n");
			break;
		}
		
		if (!quiet)
			fprintf(stderr, "simhashing %ld files ...\n", 
				(long int) B->b_DirList.dls_NumFiles);
		
		err = binsort_genhashes(B);
		if (err)
		{
			fprintf(stderr, "*** error generating hashes\n");
			break;
		}
		
		/* need min. 3 files to optimize */
		if (B->b_NumHashes > 2)
		{
			if (!quiet)
				fprintf(stderr, "calculating %ld distances ...\n", (long int)
					(B->b_NumHashes * B->b_NumHashes / 2));
			
			err = binsort_gendistances(B);
			if (err)
			{
				fprintf(stderr, "*** error calculating distances\n");
				break;
			}
			
			if (!quiet)
				fprintf(stderr, "optimizing ...        \n");
			
			err = binsort_genorder(B);
			binsort_freedistances(B);
		}
		else if (!quiet)
			fprintf(stderr, "nothing to optimize\n");
			
		
		if (err)
		{
			fprintf(stderr, "*** error optimizing\n");
			break;
		}
		else
		{
			struct Node *next, *node;
			if (!quiet)
				fprintf(stderr, "d=%ld done.              \n", 
					(long int) B->b_CurrentDistance);
			node = B->b_DirList.dls_Head.lh_Head;
			for (; (next = node->ln_Succ); node = next)
			{
				struct DirEntry *entry = (struct DirEntry *) node;
				printf("%s\n", entry->den_Name);
			}
		}

	} while (0);
	
	return err;
}

void binsort_destroy(binsort_t *B)
{
	binsort_free(B);
	free(B);
}

binsort_error_t binsort_create(binsort_t **B, binsort_argitem_t *args)
{
	binsort_error_t err = BINSORT_ERROR_OUT_OF_MEMORY;
	*B = malloc(sizeof **B);
	if (*B)
	{
		err = binsort_init(*B, args);
		if (err)
		{
			binsort_destroy(*B);
			*B = NULL;
		}
	}
	return err;
}
