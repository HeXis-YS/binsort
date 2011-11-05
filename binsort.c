/*
**	Binsort - sort files by binary similarity
**
**	Copyright (c) 2011 by Timm S. Mueller <tmueller@neoscientists.org>
**	Licensed under the 3-clause BSD license, see COPYRIGHT
**
**	Scans the contents of a directory, sorts the files by binary
**	similarity, generates a filelist and prints the list to stdout. A
**	A possible application is to pass the list to an archiving tool, e.g.:
**
**	$ binsort <dir> | tar -T- --no-recursion -czf out.tar.gz
**
**	This can improve compression rates considerably, although sorting is
**	in no way optimized for a particular compression algorithm.
**
**	This is a research project combining threshold accepting,
**	shingleprinting, and excessive multithreading. It uses simhash by Bart
**	Massey (in modified form), and Tiny Mersenne Twister by Mutsuo Saito
**	and Makoto Matsumoto. See COPYRIGHT for the respective copyrights and
**	licensing terms.
*/


#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "xpthread.h"
#include "simhash.h"
#include "tinymt32.h"


#define PROG_NAME "binsort"
#define BINSORT_DEFAULT_QUALITY 20
#define BINSORT_DEFAULT_NUMTHREADS 4


typedef int32_t num_t;
typedef long long dist_t;


typedef enum 
{
	ERR_SUCCESS = 0,
	ERR_ARGUMENTS,
	ERR_THREAD_INIT,
	ERR_THREAD_CREATE,
	ERR_DIR_OPEN,
	ERR_DIR_EXAMINE,
	ERR_FILE_EXAMINE,
	ERR_OUT_OF_MEMORY,
	ERR_FILE_OPEN,
	ERR_HASHING,
} error_t;

typedef enum 
{
	MSG_HASH,
	MSG_CALCDIST,
	MSG_OPTIMIZE,
} msgtype_t;

struct Node
{
	struct Node *ln_Succ;
	struct Node *ln_Pred;
};

struct Message
{
	struct XPMessage msg_XPMessage;
	msgtype_t msg_Type;
	void *msg_Data;
};

struct List
{
	struct Node *lh_Head;
	struct Node *lh_Tail;
	struct Node *lh_TailPred;
};

struct HashList
{
	struct List hls_Head;
	num_t hls_Num;
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
	struct HashList *hm_HashList;
	struct Distances *hm_Distances;
	struct HashNode *hm_StartNode;
	int hm_FirstIndex;
	int hm_LastIndex;
};

struct OptMessage
{
	struct Message om_Message;
	num_t om_NumEntries;
	num_t om_NumIterations;
	struct DirEntry **om_Order;
	tinymt32_t om_Random;
	struct Distances *om_Distances;
	dist_t om_InitialDistance;
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
	uint8_t den_IsFile;
	uint8_t den_IsDir;
	num_t den_Index;
	error_t den_Error;
	struct HashNode *den_HashNode;
};

struct Distances
{
	uint8_t *dst_Array;
	num_t dst_Num;
};

struct RangeNode 
{
	struct Node rn_Node;
	uint32_t rn_First, rn_Last;
};

struct Arguments
{
	const char *arg_Directory;
	int arg_Quality;
	int arg_NumThreads;
	int arg_Quiet;
	int arg_NoPrintDirs;
};

struct BinSort
{
	/* Pointer to arguments */
	struct Arguments *b_Arguments;
	/* Threading library base */
	struct XPBase *b_XPBase;
	/* Main thread context */
	struct XPThread *b_Self;
	/* Array of worker threads */
	struct XPThread **b_Workers;
	/* Message port for worker replies */
	struct XPPort *b_ReplyPort;
	/* Signal bit for worker replyport */
	XPSIGMASK b_ReplyPortSignal;
	/* List of all files, directories and other filesystem objects */
	struct DirList b_DirList;
	/* List of simhashes */
	struct HashList b_Hashes;
	/* Distances structure */
	struct Distances *b_Distances;
	/* Number of distances left to calculate */
	num_t b_DistancesLeft;
	/* Locking object for the following fields */
	struct XPFastMutex *b_Lock;
	/* Current order of file entries */
	struct DirEntry **b_Order;
	/* List of ranges currently being swapped by workers */
	struct List b_RangeList;
	/* Sum of current order's file distances */
	dist_t b_CurrentDistance;
};


#define XPT_SIG_UPDATE	0x00010000

static void binsort_freedir(struct BinSort *B);


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

static void AddTail(struct List *list, struct Node *node)
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

static void Remove(struct Node *node)
{
	struct Node *temp = node->ln_Succ;
	node->ln_Pred->ln_Succ = temp;
	temp->ln_Pred = node->ln_Pred;
}


/*
**	err = dirlist_scan(dirlist, dirname)
**	Scan directory recursively
*/

static error_t dirlist_scan(struct DirList *list, const char *dirname)
{
	struct Node *next, *node = list->dls_Head.lh_TailPred;
	int num = 0, err = ERR_SUCCESS, res = 0;
	struct dirent *dp;
	size_t pathlen = strlen(dirname);
	DIR *dir = opendir(dirname);
	if (dir == NULL)
		return ERR_DIR_OPEN;
	if (pathlen > 0 && dirname[pathlen - 1] == '/')
		pathlen--;
	while ((dp = readdir(dir)))
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
			{
				free(direntry);
				err = ERR_FILE_EXAMINE;
				break;
			}
		}
	}
	closedir(dir);
	if (res != 0)
		err = ERR_DIR_EXAMINE;
	if (!err)
	{
		int i = 0;
		node = node->ln_Succ;
		for (; i < num && (next = node->ln_Succ); node = next, ++i)
		{
			struct DirEntry *dn = (struct DirEntry *) node;
			if (dn->den_IsDir)
			{
				err = dirlist_scan(list, dn->den_Name);
				if (err)
					break;
			}
		}
	}
	return err;
}

/*
**	err = binsort_genhashes(binsort)
**	Generate hashes for all files
*/

static error_t binsort_genhashes(struct BinSort *B)
{
	struct XPBase *xpbase = B->b_XPBase;
	struct XPPort *rport = B->b_ReplyPort;
	XPSIGMASK sig, portsig = B->b_ReplyPortSignal;
	struct Node *next, *node = B->b_DirList.dls_Head.lh_Head;
	int numworkers = B->b_Arguments->arg_NumThreads;
	int sent = 0;
	int quiet = B->b_Arguments->arg_Quiet;
	
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
	
	do
	{
		struct XPMessage *msg;
		sig = (*xpbase->wait)(xpbase, portsig);
		while ((msg = (*xpbase->getmsg)(xpbase, rport)))
		{
			struct DirEntry *direntry = ((struct Message *) msg)->msg_Data;
			struct HashNode *hashnode = direntry->den_HashNode;
			if (hashnode)
			{
				direntry->den_Index = B->b_Hashes.hls_Num++;
				AddTail(&B->b_Hashes.hls_Head, &hashnode->hn_Node);
			}
			if ((--sent & 127) == 0 && !quiet)
				fprintf(stderr, "%d files left   \r", sent);
		}
	} while (sent > 0);
	
	return ERR_SUCCESS;
}

/*
**	err = binsort_gendistances(binsort)
**	Calculate distance array
*/

error_t binsort_gendistances(struct BinSort *B)
{
	struct XPBase *xpbase = B->b_XPBase;
	struct XPPort *rport = B->b_ReplyPort;
	XPSIGMASK portsig = B->b_ReplyPortSignal;
	int numworkers = B->b_Arguments->arg_NumThreads;
	struct HashList *hashes = &B->b_Hashes;
	num_t num = hashes->hls_Num;
	struct HashMessage *msgs = malloc(sizeof *msgs * numworkers);
	if (!msgs)
		return ERR_OUT_OF_MEMORY;
	
	/* avoid overproportional number of workers to distances: */
	if (num / 10 < numworkers)
		numworkers = num / 10;
	
	do
	{
		int y, i, i0;
		struct Node *ynext, *ynode = hashes->hls_Head.lh_Head;
		B->b_DistancesLeft = (num - 1) * (num - 1) / 2;
		double a0 = num * num / numworkers;
		struct Distances *d = malloc(sizeof *d + num * num);
		if (d == NULL)
			break;
		d->dst_Num = num;
		d->dst_Array = (uint8_t *) (d + 1);
		for (i = i0 = y = 0; (ynext = ynode->ln_Succ); ynode = ynext, ++y)
		{
			if (y == i0)
			{
				struct HashNode *yhash = (struct HashNode *) ynode;
				struct XPThread *worker = B->b_Workers[i];
				struct XPPort *port = (*xpbase->getuserport)(xpbase, worker);
				int i1 = num - 1;
				if (i < numworkers - 1)
					i1 = floor(sqrt((i + 1) * a0)) - 1;
				msgs[i].hm_Message.msg_Type = MSG_CALCDIST;
				msgs[i].hm_Message.msg_Data = &msgs[i];
				msgs[i].hm_HashList = hashes;
				msgs[i].hm_StartNode = yhash;
				msgs[i].hm_FirstIndex = i0;
				msgs[i].hm_LastIndex = i1;
				msgs[i].hm_Distances = d;
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
		if (!B->b_Arguments->arg_Quiet && (sig & XPT_SIG_UPDATE))
		{
			fprintf(stderr, "%d deltas left           \r", 
				B->b_DistancesLeft);
		}
	} while (numworkers > 0);
	free(msgs);
	return ERR_SUCCESS;
}


/*
**	optimization helpers
*/

static int getdeltaindex(int max, int idx, int d)
{
	idx += d;
	if (idx < 0)
		return idx + max;
	return idx % max;
}

static int getdistd(struct DirEntry **order, int num,
	struct Distances *distances, int i0, int i1, int delta)
{
	int a, b;
	assert(i0 >= 0 && i0 < num);
	assert(i1 >= 0 && i1 < num);
	a = order[i0]->den_Index;
	if (a < 0)
		return 0;
	b = order[getdeltaindex(num, i1, delta)]->den_Index;
	if (b < 0)
		return 0;
	return distances->dst_Array[a * distances->dst_Num + b];
}

static dist_t getdist(struct DirEntry **order, int num, 
	struct Distances *distances)
{
	dist_t d = 0;
	int i0;
	for (i0 = 0; i0 < num; ++i0)
		d += getdistd(order, num, distances, i0, i0, 1);
	return d;
}

static dist_t getdelta(struct DirEntry **order, int num, 
	struct Distances *distances, int i0, int i1)
{
	return 
		- getdistd(order, num, distances, i0, i0, -1)
		- getdistd(order, num, distances, i1, i1, 1)
		+ getdistd(order, num, distances, i0, i1, 1)
		+ getdistd(order, num, distances, i1, i0, -1);
}


/*
**	generate order - main function of optimization
*/

static error_t binsort_genorder(struct BinSort *B)
{
	struct XPBase *xpbase = B->b_XPBase;
	struct XPPort *rport = B->b_ReplyPort;
	XPSIGMASK portsig = B->b_ReplyPortSignal;
	struct DirList *dlist = &B->b_DirList;
	struct Distances *distances = B->b_Distances;
	int num, i;
	dist_t d;
	struct Node *next, *node;
	struct DirEntry **order;
	int numworkers = B->b_Arguments->arg_NumThreads;
	int quality = B->b_Arguments->arg_Quality;
	struct OptMessage *msgs = malloc(sizeof *msgs * numworkers);
	if (!msgs)
		return ERR_OUT_OF_MEMORY;
	
	B->b_Order = order = malloc(sizeof *order * dlist->dls_NumFiles);
	if (order == NULL)
		return ERR_OUT_OF_MEMORY;

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

	/* distribute optimization to workers: */
	d = getdist(order, num, distances);
	B->b_CurrentDistance = d;
	
	for (i = 0; i < numworkers; ++i)
	{
		struct XPThread *worker = B->b_Workers[i];
		struct XPPort *port = (*xpbase->getuserport)(xpbase, worker);
		msgs[i].om_Message.msg_Type = MSG_OPTIMIZE;
		msgs[i].om_Message.msg_Data = &msgs[i];
		msgs[i].om_NumEntries = num;
		msgs[i].om_Order = order;
		memset(&msgs[i].om_Random, 0x5a, sizeof msgs[i].om_Random);
		tinymt32_init(&msgs[i].om_Random, 4567 + i);
		msgs[i].om_Distances = distances;
		msgs[i].om_InitialDistance = d * 5;
		msgs[i].om_NumIterations = pow(d, 1.1) * quality / numworkers;
		(*xpbase->putmsg)(xpbase, port, rport, 
			&msgs[i].om_Message.msg_XPMessage);
	}
	do
	{
		XPSIGMASK sig = (*xpbase->wait)(xpbase, portsig | XPT_SIG_UPDATE);
		while ((*xpbase->getmsg)(xpbase, rport))
			--numworkers;
		if (!B->b_Arguments->arg_Quiet && (sig & XPT_SIG_UPDATE))
			fprintf(stderr, "d=%lld         \r", B->b_CurrentDistance);
	} while (numworkers > 0);

	assert(B->b_CurrentDistance == getdist(order, num, distances));

	if (B->b_Arguments->arg_NoPrintDirs)
		binsort_freedir(B);
	
	/* add files back to list: */
	for (i = 0; i < num; ++i)
		AddTail(&dlist->dls_Head, &order[i]->den_Node);

	free(msgs);
	return ERR_SUCCESS;
}


/*
**	optimization worker
*/

static void binsort_worker_optimize(struct XPBase *xpbase, struct BinSort *B,
	struct OptMessage *msg)
{
	struct DirEntry **order = msg->om_Order;
	tinymt32_t *random = &msg->om_Random;
	num_t num = msg->om_NumEntries;
	struct Distances *distances = B->b_Distances;
	struct List *rangelist = &B->b_RangeList;
	struct XPFastMutex *lock = B->b_Lock;
	dist_t delta;
	struct Node *node, *next;
	num_t i0, i1, n, i;
	double dunk = 
		(double) msg->om_InitialDistance * 1.25 / msg->om_NumIterations;
	
	for (i = 0; i < msg->om_NumIterations; ++i)
	{
		double thresh = (double) msg->om_InitialDistance / i - dunk;

		if ((i & 65535) == 0)
			(*xpbase->signal)(xpbase, B->b_Self, XPT_SIG_UPDATE);

		again:
		
		i0 = tinymt32_generate_uint32(random) % num;
		i1 = tinymt32_generate_uint32(random) % num;
		n = 0;

		if (i0 > i1 + 2)
		{
			n = num - i0 + i1 + 1;
			if (n > i0 - i1 - 1)
			{
				int t = i1 + 1;
				i1 = i0 - 1;
				i0 = t;
				n = i1 - i0 + 1;
			}
		}
		else if (i1 > i0 && i1 - i0 < num - 2)
		{
			if (num - i1 + i0 - 1 < i1 - i0 + 1)
			{
				int t = getdeltaindex(num, i0, -1);
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
		
		delta = getdelta(order, num, distances, i0, i1);
		if (delta <= (thresh < 0 ? 0 : thresh))
		{
			struct RangeNode rangelock;
			struct DirEntry *t;
			num_t i;

			rangelock.rn_First = i0;
			rangelock.rn_Last = i1;
			AddTail(rangelist, &rangelock.rn_Node);

			t = order[i0];
			order[i0] = order[i1];
			order[i1] = t;
			i0 = (i0 + 1) % num;
			i1 = getdeltaindex(num, i1, -1);

			(*xpbase->unlockfastmutex)(xpbase, lock);

			for (i = 1; i < n / 2; ++i)
			{
				t = order[i0];
				order[i0] = order[i1];
				order[i1] = t;
				i0 = (i0 + 1) % num;
				i1 = getdeltaindex(num, i1, -1);
			}

			(*xpbase->lockfastmutex)(xpbase, lock);

			Remove(&rangelock.rn_Node);
			B->b_CurrentDistance += delta;
		}
		
		(*xpbase->unlockfastmutex)(xpbase, lock);
	}
}


/*
**	simhash worker
*/

static void binsort_worker_hash(struct Message *msg)
{
	error_t err = ERR_FILE_OPEN;
	struct DirEntry *direntry = msg->msg_Data;
	FILE *f = fopen(direntry->den_Name, "rb");
	if (f)
	{
		struct simhash *h;
		err = ERR_HASHING;
		h = simhash_file(f);
		fclose(f);
		if (h)
		{
			size_t hsize;
			const uint8_t *hash = simhash_get(h, &hsize);
			struct HashNode *hn = malloc(sizeof *hn + hsize);
			err = ERR_OUT_OF_MEMORY;
			if (hn)
			{
				hn->hn_Hash = (uint8_t *) (hn + 1);
				memcpy(hn->hn_Hash, hash, hsize);
				hn->hn_Size = hsize;
				err = ERR_SUCCESS;
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
**	delta calculation worker
*/

static void binsort_worker_calcdist(struct XPBase *xpbase, struct BinSort *B,
	struct HashMessage *msg)
{
	int y, x, i = 0;
	struct Node *ynext, *ynode = (struct Node *) msg->hm_StartNode;
	struct HashList *hashes = msg->hm_HashList;
	size_t num = hashes->hls_Num;
	struct Distances *d = msg->hm_Distances;
	for (y = msg->hm_FirstIndex; 
		y <= msg->hm_LastIndex && (ynext = ynode->ln_Succ); 
		ynode = ynext, ++y)
	{
		struct HashNode *yhash = (struct HashNode *) ynode;
		struct Node *xnext, *xnode = hashes->hls_Head.lh_Head;
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
			d->dst_Array[x + y * num] = vali;
			d->dst_Array[y + x * num] = vali;
			if ((++i & 262143) == 0)
			{
				(*xpbase->lockfastmutex)(xpbase, B->b_Lock);
				B->b_DistancesLeft -= 262143;
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
	struct BinSort *B = (*xpbase->getdata)(xpbase, self);
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
				{
					binsort_worker_hash(msg);
					break;
				}
				case MSG_CALCDIST:
				{
					binsort_worker_calcdist(xpbase, B,
						(struct HashMessage *) msg);
					break;
				}
				case MSG_OPTIMIZE:
				{
					binsort_worker_optimize(xpbase, B,
						(struct OptMessage *) msg);
					break;
				}
			}
			(*xpbase->replymsg)(xpbase, xpmsg);
		}
	} while (!(sig & XPT_SIG_ABORT));
}


/*
**	init, free
*/

static void binsort_freeworkers(struct BinSort *B)
{
	if (B->b_Workers)
	{
		int nt = B->b_Arguments->arg_NumThreads;
		int i;
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

static error_t binsort_initworkers(struct BinSort *B)
{
	int nt = B->b_Arguments->arg_NumThreads;
	int i;
	B->b_Workers = malloc(sizeof *B->b_Workers * nt);
	for (i = 0; i < nt; ++i)
	{
		B->b_Workers[i] = (*B->b_XPBase->createthread)(B->b_XPBase,
			binsort_worker, B, NULL);
		if (B->b_Workers[i] == NULL)
		{
			binsort_freeworkers(B);
			return ERR_THREAD_CREATE;
		}
	}
	return ERR_SUCCESS;
}

static error_t binsort_init(struct BinSort *B, struct Arguments *args)
{
	struct XPBase *xpbase;
	memset(B, 0, sizeof *B);
	B->b_Arguments = args;
	InitList(&B->b_DirList.dls_Head);
	InitList(&B->b_Hashes.hls_Head);
	InitList(&B->b_RangeList);
	B->b_XPBase = xpbase = xpthread_create(NULL);
	if (xpbase == NULL)
		return ERR_THREAD_INIT;
	B->b_Lock = (*xpbase->createfastmutex)(xpbase);
	if (B->b_Lock == NULL)
		return ERR_THREAD_INIT;
	B->b_Self = (*xpbase->findthread)(xpbase, NULL);
	B->b_ReplyPort = (*xpbase->getuserport)(xpbase, B->b_Self);
	B->b_ReplyPortSignal = (*xpbase->getportsignal)(xpbase, B->b_ReplyPort);
	return binsort_initworkers(B);
}

static void binsort_freedir(struct BinSort *B)
{
	struct Node *node;
	while ((node = RemTail(&B->b_DirList.dls_Head)))
		free(node);
}

static void binsort_freehashes(struct BinSort *B)
{
	struct Node *node;
	while ((node = RemTail(&B->b_Hashes.hls_Head)))
		free(node);
	B->b_Hashes.hls_Num = 0;
}

static void binsort_freedistances(struct BinSort *B)
{
	if (B->b_Distances != NULL)
		free(B->b_Distances);
	B->b_Distances = NULL;
}

static void binsort_free(struct BinSort *B)
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
**	err = binsort_run(binsort, dirname)
**	binsort main procedure
*/

static error_t binsort_run(struct BinSort *B, const char *dirname)
{
	error_t err;
	int quiet = B->b_Arguments->arg_Quiet;
	do
	{
		err = dirlist_scan(&B->b_DirList, dirname);
		if (err)
		{
			fprintf(stderr, "*** error scanning directory\n");
			break;
		}
		
		if (!quiet)
			fprintf(stderr, "simhashing %d files ...\n", 
				B->b_DirList.dls_NumFiles);
		
		err = binsort_genhashes(B);
		if (err)
		{
			fprintf(stderr, "*** error generating hashes\n");
			break;
		}
		
		if (!quiet)
			fprintf(stderr, "calculating %d deltas ...\n",
				B->b_Hashes.hls_Num * B->b_Hashes.hls_Num / 2);
		
		err = binsort_gendistances(B);
		if (err)
		{
			fprintf(stderr, "*** error calculating distances\n");
			break;
		}
		
		if (!quiet)
			fprintf(stderr, "optimizing ...      \n");
		
		err = binsort_genorder(B);
		binsort_freedistances(B);
		
		if (err)
		{
			fprintf(stderr, "*** error optimizing\n");
			break;
		}
		else
		{
			struct Node *next, *node;
			if (!quiet)
				fprintf(stderr, "d=%lld done.              \n", 
					B->b_CurrentDistance);
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


/*
**	main
*/

typedef struct { const char *key; void *val; void *valptr; char type; } arg_t;

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
				args[n].val = args[n].valptr;
				*((int *) args[n].valptr) = atoi(arg);
				wait = 0;
				continue;
		}
		for (j = 0; j < numargs; ++j)
		{
			if (args[j].val)
				continue;
			if (args[j].key)
			{
				if (!strcmp(arg, args[j].key))
				{
					n = j;
					if (args[j].type == 'n')
						wait = 'n';
					else if (args[j].type == 'b')
						*((int *) args[n].valptr) = 1;
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
				args[k].val = args[k].valptr;
				*((const char **) args[k].valptr) = arg;
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
	struct Arguments args = 
		{ NULL, BINSORT_DEFAULT_QUALITY, BINSORT_DEFAULT_NUMTHREADS, 0, 0 };
	arg_t argparse[] = 
	{ 
		{ NULL, NULL, &args.arg_Directory, 's' },
		{ "-o", NULL, &args.arg_Quality, 'n' },
		{ "-t", NULL, &args.arg_NumThreads, 'n' },
		{ "-q", NULL, &args.arg_Quiet, 'b' },
		{ "-d", NULL, &args.arg_NoPrintDirs, 'b' },
		{ "-h", NULL, &help, 'b' },
		{ "--help", NULL, &help, 'b'},
	};
	if (parseargs(argc, argv, argparse, 7) && !help)
	{
		do
		{
			struct BinSort binsort, *B = &binsort;
			error_t err = ERR_ARGUMENTS;
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
			if (args.arg_NumThreads < 1 || args.arg_NumThreads > 128)
			{
				printf(PROG_NAME 
					": Number of threads must be between 1 and 128\n");
				break;
			}
			err = binsort_init(B, &args);
			if (!err)
				err = binsort_run(B, args.arg_Directory);
			binsort_free(B);
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
