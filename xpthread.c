
/*
**	xpthread - Cross-platform multithreading library
**	Written 2003-2008 by Timm S. Mueller <tmueller@neoscientists.org>
**	Placed in the public domain, no copyrights apply.
*/

#include "xpthread.h"

#define VERSION		2
#define REVISION	0

#ifndef EXPORT
#define EXPORT
#endif

/*****************************************************************************/

#if defined(XPT_PTHREADS)

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>

typedef pthread_mutex_t XPMUTEX;
#define XPT_MUTEX_INIT(lock) (pthread_mutex_init((lock), NULL) == 0)
#define XPT_MUTEX_DESTROY(lock) pthread_mutex_destroy(lock)
#define XPT_MUTEX_LOCK(lock) pthread_mutex_lock(lock)
#define XPT_MUTEX_TRYLOCK(lock) pthread_mutex_trylock(lock)
#define XPT_MUTEX_UNLOCK(lock) pthread_mutex_unlock(lock)

typedef void *XPTHREADENTRY;
typedef pthread_cond_t XPEVENT;
typedef pthread_t XPTHREAD;
typedef pthread_key_t XPTSLKEY;
#define XPT_TLS_INIT(key) (pthread_key_create(key, NULL) == 0)
#define XPT_TLS_DESTROY(key) pthread_key_delete(*(key))
#define XPT_TLS_SET(key, val) pthread_setspecific(key, (void *) (val))
#define XPT_TLS_GET(key) pthread_getspecific(key)
#define XPT_EVENT_INIT(event) (pthread_cond_init(event, NULL) == 0)
#define XPT_EVENT_DESTROY(event) pthread_cond_destroy(event)
#define XPT_EVENT_SIGNAL(event) pthread_cond_signal(event)
#define XPT_EVENT_WAIT(event, lock) (pthread_cond_wait(event, lock), 0)
#define XPT_THREAD_INIT(thread, func, data) \
	(pthread_create(thread, NULL, (void *(*)(void *)) (func), data) == 0)
#define XPT_THREAD_DESTROY(thread) (pthread_join(*(thread), NULL), 0)
#define XPT_THREAD_EXIT() (0)

#else

#error platform not supported

#endif

/*****************************************************************************/

typedef enum { XPT_INIT, XPT_RUN, XPT_DEAD } XPSTATUS;

struct XPQueue
{
	struct XPMessage *xpmq_Head;
	struct XPMessage *xpmq_Tail;
	struct XPMessage *xpmq_TailPred;
};

struct XPThread
{
	struct XPMessage xpt_Node;
	struct XPBase *xpt_Base;
	struct XPPort *xpt_SyncPort;
	struct XPPort *xpt_UserPort;
	struct XPThread *xpt_Parent;
	char *xpt_Name;
	XPSTATUS xpt_Status;
	size_t xpt_RefCount;
	XPSIGMASK xpt_SigFree;
	XPSIGMASK xpt_SigUsed;
	XPSIGMASK xpt_SigState;
	void (*xpt_Function)(struct XPBase *);
	void *xpt_UserData;
	XPMUTEX xpt_SigLock;
	XPEVENT xpt_SigEvent;
	XPTHREAD xpt_Thread;
};

struct XPPort
{
	/* Message queue: */
	struct XPQueue xpmp_Queue;
	/* Protection for queue: */
	XPMUTEX xpmp_Lock;
	/* Thread owning this port: */
	struct XPThread *xpmp_SigThread;
	/* Signal bit reserved for : */
	XPSIGMASK xpmp_Signal;
};

struct XPThreadInternal
{
	/* List of tasks: */
	struct XPQueue xpi_Tasks;
	/* Thread handle of base context: */
	struct XPThread *xpi_BaseThread;
	/* Thread local storage key: */
	XPTSLKEY xpi_TLSKey;
	/* Protection for list of tasks: */
	XPMUTEX xpi_BaseLock;
};

struct XPNode
{
	struct XPNode *xpln_Succ;
	struct XPNode *xpln_Pred;
};

struct XPLock
{
	struct XPNode xplk_Node;
	struct XPQueue xplk_Waiters;
	XPMUTEX xplk_Mutex;
	struct XPThread *xplk_Owner;
	int16_t xplk_Count;
};

struct XPLockWait
{
	struct XPNode xplw_Node;
	struct XPThread *xplw_Task;
	int xplw_Shared;
};

/*****************************************************************************/
/*
**	Platform-specific:
*/

#if defined(XPT_PTHREADS)

static pthread_once_t xpt_initialized = PTHREAD_ONCE_INIT;
static struct XPBase *xptbase = NULL;
static void *xpt_initargs;

static void xpi_deinitialize(void)
{
	xpthread_destroy(xptbase);
}

static void xpi_initialize(void)
{
	xptbase = xpthread_create(xpt_initargs);
	if (xptbase)
		atexit(xpi_deinitialize);
}

EXPORT struct XPBase *xpthread_create_once(void *args)
{
	xpt_initargs = args;
	pthread_once(&xpt_initialized, xpi_initialize);
	return xptbase;
}

static int xpi_timedwait(struct XPBase *xpbase, XPEVENT *event,
	XPMUTEX *lock, const struct XPTime *time)
{
	struct timespec tv;
	tv.tv_sec = time->xptm_Sec;
	tv.tv_nsec = time->xptm_USec * 1000;
	return (pthread_cond_timedwait(event, lock, &tv) == ETIMEDOUT);
}

static void xp_gettime(struct XPBase *xpbase, struct XPTime *time)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	time->xptm_Sec = tv.tv_sec;
	time->xptm_USec = tv.tv_usec;
}

#else

#error platform not supported

#endif

/*****************************************************************************/
/*
**	Platform-independent:
*/

#define ISLISTEMPTY(l) ((l)->xpmq_TailPred == (void *) (l))

static void xpi_initlist(struct XPQueue *list)
{
	list->xpmq_TailPred = (struct XPMessage *) list;
	list->xpmq_Tail = NULL;
	list->xpmq_Head = (struct XPMessage *) &list->xpmq_Tail;
}

static void xpi_addtail(struct XPQueue *list, struct XPMessage *node)
{
	struct XPMessage *temp = list->xpmq_TailPred;
	list->xpmq_TailPred = node;
	node->xpmsg_Succ = (struct XPMessage *) &list->xpmq_Tail;
	node->xpmsg_Pred = temp;
	temp->xpmsg_Succ = node;
}

static struct XPMessage *xpi_remhead(struct XPQueue *list)
{
	struct XPMessage *temp = list->xpmq_Head;
	if (temp->xpmsg_Succ == NULL) return NULL;
	list->xpmq_Head = temp->xpmsg_Succ;
	temp->xpmsg_Succ->xpmsg_Pred = (struct XPMessage *) &list->xpmq_Head;
	return temp;
}

static void xpi_remove(struct XPMessage *node)
{
	node->xpmsg_Pred->xpmsg_Succ = node->xpmsg_Succ;
	node->xpmsg_Succ->xpmsg_Pred = node->xpmsg_Pred;
}

static XPSIGMASK xp_allocsignal(struct XPBase *xpbase,
	struct XPThread *xpt, XPSIGMASK signals)
{
	XPSIGMASK newsignal = 0;
	XPT_MUTEX_LOCK(&xpt->xpt_SigLock);
	if (signals)
	{
		if ((signals & xpt->xpt_SigFree) == signals)
			newsignal = signals;
	}
	else
	{
		XPSIGMASK trysignal = 0x00000001;
		int x;
		for (x = 0; x < 32; ++x)
		{
			if (!(trysignal & XPT_SIG_RESERVED) &&
				(trysignal & xpt->xpt_SigFree))
			{
				newsignal = trysignal;
				break;
			}
			trysignal <<= 1;
		}
	}
	xpt->xpt_SigFree &= ~newsignal;
	xpt->xpt_SigUsed |= newsignal;
	XPT_MUTEX_UNLOCK(&xpt->xpt_SigLock);
	return newsignal;
}

static void xp_freesignal(struct XPBase *xpbase, struct XPThread *xpt,
	XPSIGMASK signals)
{
	XPT_MUTEX_LOCK(&xpt->xpt_SigLock);
	xpt->xpt_SigFree |= signals;
	xpt->xpt_SigUsed &= ~signals;
	XPT_MUTEX_UNLOCK(&xpt->xpt_SigLock);
}

static struct XPPort *xp_createport(struct XPBase *xpbase,
	struct XPThread *xpt, XPSIGMASK prefsig)
{
	struct XPPort *xpp = malloc(sizeof(struct XPPort));
	if (xpp)
	{
		if (XPT_MUTEX_INIT(&xpp->xpmp_Lock))
		{
			xpp->xpmp_Signal = prefsig & XPT_SIG_RESERVED ? prefsig:
				xp_allocsignal(xpbase, xpt, prefsig);
			if (xpp->xpmp_Signal)
			{
				xpi_initlist(&xpp->xpmp_Queue);
				xpp->xpmp_SigThread = xpt;
				return xpp;
			}
			XPT_MUTEX_DESTROY(&xpp->xpmp_Lock);
		}
		free(xpp);
	}
	return NULL;
}

static void xp_destroyport(struct XPBase *xpbase, struct XPPort *xpp)
{
	if (xpp)
	{
		xp_freesignal(xpbase, xpp->xpmp_SigThread, xpp->xpmp_Signal);
		XPT_MUTEX_DESTROY(&xpp->xpmp_Lock);
		free(xpp);
	}
}

static struct XPThread *xpi_findrefthread(struct XPBase *xpbase,
	const char *name, int refincrement)
{
	struct XPThreadInternal *xpi = xpbase->internal;
	struct XPMessage *msg, *next;
	XPT_MUTEX_LOCK(&xpi->xpi_BaseLock);
	msg = xpi->xpi_Tasks.xpmq_Head;
	for (; (next = msg->xpmsg_Succ); msg = next)
	{
		struct XPThread *xpt = (struct XPThread *) msg;
		if (xpt->xpt_Status != XPT_DEAD && strcmp(xpt->xpt_Name, name) == 0)
		{
			xpt->xpt_RefCount += refincrement;
			XPT_MUTEX_UNLOCK(&xpi->xpi_BaseLock);
			return xpt;
		}
	}
	XPT_MUTEX_UNLOCK(&xpi->xpi_BaseLock);
	return NULL;
}

static struct XPThread *xp_findthread(struct XPBase *xpbase,
	const char *name)
{
	if (name == NULL)
	{
		struct XPThreadInternal *xpi = xpbase->internal;
		return (struct XPThread *) XPT_TLS_GET(xpi->xpi_TLSKey);
	}
	return xpi_findrefthread(xpbase, name, 0);
}

static int xp_renamethread(struct XPBase *xpbase, struct XPThread *xpt,
	const char *newname)
{
	struct XPThreadInternal *xpi = xpbase->internal;
	size_t nlen = newname ? strlen(newname) : 0;
	char *nname = malloc(nlen + 1);
	if (nname)
	{
		char *oldname;
		strncpy(nname, newname, nlen);
		nname[nlen] = 0;
		XPT_MUTEX_LOCK(&xpi->xpi_BaseLock);
		oldname = xpt->xpt_Name;
		xpt->xpt_Name = nname;
		XPT_MUTEX_UNLOCK(&xpi->xpi_BaseLock);
		free(oldname);
		return 0;
	}
	return -1; /* error: out of memory */
}

static size_t xp_getname(struct XPBase *xpbase, struct XPThread *xpt,
	char *namebuf, size_t namebuf_len)
{
	struct XPThreadInternal *xpi = xpbase->internal;
	size_t name_len;
	size_t result = 0; /* success / how many bytes are needed for name */

	XPT_MUTEX_LOCK(&xpi->xpi_BaseLock);
	name_len = xpt->xpt_Name ? strlen(xpt->xpt_Name) : 0;
	if (namebuf == NULL)
		result = name_len + 1;
	else if (namebuf_len > name_len)
	{
		strncpy(namebuf, xpt->xpt_Name, name_len);
		namebuf[name_len] = 0;
		result = name_len + 1;
	}
	XPT_MUTEX_UNLOCK(&xpi->xpi_BaseLock);

	return result;
}

static void *xp_getdata(struct XPBase *xpbase, struct XPThread *xpt)
{
	return xpt->xpt_UserData;
}

static void *xp_setdata(struct XPBase *xpbase, struct XPThread *xpt,
	void *udata)
{
	void *old = xpt->xpt_UserData;
	xpt->xpt_UserData = udata;
	return old;
}

static void xp_signal(struct XPBase *xpbase, struct XPThread *xpt,
	XPSIGMASK signals)
{
	XPT_MUTEX_LOCK(&xpt->xpt_SigLock);
	if (signals & ~xpt->xpt_SigState)
	{
		xpt->xpt_SigState |= signals;
		XPT_EVENT_SIGNAL(&xpt->xpt_SigEvent);
	}
	XPT_MUTEX_UNLOCK(&xpt->xpt_SigLock);
}

static XPSIGMASK xp_setsignal(struct XPBase *xpbase, XPSIGMASK newsig,
	XPSIGMASK sigmask)
{
	struct XPThread *xpt = xp_findthread(xpbase, NULL);
	XPSIGMASK oldsig;
	XPT_MUTEX_LOCK(&xpt->xpt_SigLock);
	oldsig = xpt->xpt_SigState;
	xpt->xpt_SigState &= ~sigmask;
	xpt->xpt_SigState |= newsig;
	if ((newsig & sigmask) & ~oldsig)
		XPT_EVENT_SIGNAL(&xpt->xpt_SigEvent);
	XPT_MUTEX_UNLOCK(&xpt->xpt_SigLock);
	return oldsig;
}

static XPSIGMASK xp_wait(struct XPBase *xpbase, XPSIGMASK sigmask)
{
	struct XPThread *xpt = xp_findthread(xpbase, NULL);
	XPSIGMASK sig;
	XPT_MUTEX_LOCK(&xpt->xpt_SigLock);
	for (;;)
	{
		sig = xpt->xpt_SigState & sigmask;
		xpt->xpt_SigState &= ~sigmask;
		if (sig)
			break;
		XPT_EVENT_WAIT(&xpt->xpt_SigEvent, &xpt->xpt_SigLock);
	}
	XPT_MUTEX_UNLOCK(&xpt->xpt_SigLock);
	return sig;
}

static XPSIGMASK xp_timedwait(struct XPBase *xpbase, XPSIGMASK sigmask,
	const struct XPTime *time)
{
	struct XPThread *xpt = xp_findthread(xpbase, NULL);
	XPSIGMASK sig;
	XPT_MUTEX_LOCK(&xpt->xpt_SigLock);
	for (;;)
	{
		sig = xpt->xpt_SigState & sigmask;
		xpt->xpt_SigState &= ~sigmask;
		if (sig || xpi_timedwait(xpbase, &xpt->xpt_SigEvent,
			&xpt->xpt_SigLock, time))
			break;
	}
	XPT_MUTEX_UNLOCK(&xpt->xpt_SigLock);
	return sig;
}

static struct XPPort *xp_getuserport(struct XPBase *xpbase,
	struct XPThread *xpt)
{
	return xpt->xpt_UserPort;
}

static XPSIGMASK xp_getportsignal(struct XPBase *xpbase,
	struct XPPort *xpp)
{
	return xpp->xpmp_Signal;
}

static void xp_putmsg(struct XPBase *xpbase, struct XPPort *port,
	struct XPPort *replyport, struct XPMessage *msg)
{
	XPT_MUTEX_LOCK(&port->xpmp_Lock);
	msg->xpmsg_ReplyPort = replyport;
	xpi_addtail(&port->xpmp_Queue, msg);
	XPT_MUTEX_UNLOCK(&port->xpmp_Lock);
	xp_signal(xpbase, port->xpmp_SigThread, port->xpmp_Signal);
}

static struct XPMessage *xp_getmsg(struct XPBase *xpbase,
	struct XPPort *port)
{
	struct XPMessage *msg;
	XPT_MUTEX_LOCK(&port->xpmp_Lock);
	msg = xpi_remhead(&port->xpmp_Queue);
	XPT_MUTEX_UNLOCK(&port->xpmp_Lock);
	return msg;
}

static void xp_replymsg(struct XPBase *xpbase, struct XPMessage *msg)
{
	if (msg->xpmsg_ReplyPort)
		xp_putmsg(xpbase, msg->xpmsg_ReplyPort, NULL, msg);
	else
		free(msg);
}

static struct XPThread *xp_refthread(struct XPBase *xpbase,
	const char *name)
{
	if (name == NULL)
		return NULL;
	return xpi_findrefthread(xpbase, name, 1);
}

static void xp_unrefthread(struct XPBase *xpbase, struct XPThread *xpt)
{
	struct XPThreadInternal *xpi = xpbase->internal;
	XPT_MUTEX_LOCK(&xpi->xpi_BaseLock);
	if (--xpt->xpt_RefCount == 0 && xpt->xpt_Parent)
		xp_signal(xpbase, xpt->xpt_Parent, XPT_SIG_SYNC);
	XPT_MUTEX_UNLOCK(&xpi->xpi_BaseLock);
}

static void xp_subtime(struct XPBase *xpbase, struct XPTime *a,
	const struct XPTime *b)
{
	if (a->xptm_USec < b->xptm_USec)
	{
		a->xptm_Sec = a->xptm_Sec - b->xptm_Sec - 1;
		a->xptm_USec = 1000000 - (b->xptm_USec - a->xptm_USec);
	}
	else
	{
		a->xptm_Sec = a->xptm_Sec - b->xptm_Sec;
		a->xptm_USec = a->xptm_USec - b->xptm_USec;
	}
}

static void xp_addtime(struct XPBase *xpbase, struct XPTime *a,
	const struct XPTime *b)
{
	a->xptm_Sec += b->xptm_Sec;
	a->xptm_USec += b->xptm_USec;
	if (a->xptm_USec >= 1000000)
	{
		a->xptm_USec -= 1000000;
		a->xptm_Sec++;
	}
}

static int xp_cmptime(struct XPBase *xpbase, const struct XPTime *a,
	const struct XPTime *b)
{
	if (a->xptm_Sec < b->xptm_Sec)
		return -1;
	if (a->xptm_Sec > b->xptm_Sec)
		return 1;
	if (a->xptm_USec == b->xptm_USec)
		return 0;
	if (a->xptm_USec > b->xptm_USec)
		return 1;
	return -1;
}


static struct XPFastMutex *xp_createfastmutex(struct XPBase *xpbase)
{
	XPMUTEX *fastmutex = malloc(sizeof *fastmutex);
	if (XPT_MUTEX_INIT(fastmutex))
		return (struct XPFastMutex *) fastmutex;
	free(fastmutex);
	return NULL;
}

static void xp_destroyfastmutex(struct XPBase *xpbase, 
	struct XPFastMutex *fastmutex)
{
	XPT_MUTEX_DESTROY((XPMUTEX *) fastmutex);
	free(fastmutex);
}

static void xp_lockfastmutex(struct XPBase *xpbase, 
	struct XPFastMutex *fastmutex)
{
	int i;
	for (i = 0; i < 2000; ++i)
		if (XPT_MUTEX_TRYLOCK((XPMUTEX *) fastmutex) == 0)
			return;
	XPT_MUTEX_LOCK((XPMUTEX *) fastmutex);
}

static int xp_trylockfastmutex(struct XPBase *xpbase, 
	struct XPFastMutex *fastmutex)
{
	return (XPT_MUTEX_TRYLOCK((XPMUTEX *) fastmutex) == 0);
}

static void xp_unlockfastmutex(struct XPBase *xpbase, 
	struct XPFastMutex *fastmutex)
{
	XPT_MUTEX_UNLOCK((XPMUTEX *) fastmutex);
}


static struct XPLock *xp_createlock(struct XPBase *xpbase)
{
	struct XPLock *lock = malloc(sizeof *lock);
	if (lock)
	{
		xpi_initlist(&lock->xplk_Waiters);
		if (XPT_MUTEX_INIT(&lock->xplk_Mutex))
		{
			lock->xplk_Owner = NULL;
			lock->xplk_Count = 0;
			return lock;
		}
		free(lock);
	}
	return NULL;
}

static void xp_destroylock(struct XPBase *xpbase, struct XPLock *lock)
{
	XPT_MUTEX_DESTROY(&lock->xplk_Mutex);
	free(lock);
}

static void xp_lock_internal(struct XPBase *xpbase, struct XPLock *lock,
	struct XPThread *self, int shared)
{
	struct XPLockWait request;
	struct XPThread *owner;
	int count;

	while ((XPT_MUTEX_TRYLOCK(&lock->xplk_Mutex)));

	owner = lock->xplk_Owner;
	count = lock->xplk_Count;
	
	if (owner == NULL && count == 0)
	{
		/* lock is free */
		lock->xplk_Count = 1;
		if (!shared)
			lock->xplk_Owner = self;
		XPT_MUTEX_UNLOCK(&lock->xplk_Mutex);
		return; /* now locked shared or exclusively */
	}
	
	/* lock is held */
	if (owner == NULL && count > 0)
	{
		/* lock is shared */
		if (shared)
		{
			/* shared lock wanted */
			lock->xplk_Count++;
			XPT_MUTEX_UNLOCK(&lock->xplk_Mutex);
			return;	/* nesting shared lock */
		}
		/* turn shared into exclusive lock: this does not work */
		*((int *) 0) = 0;
	}
	
	/* lock is exclusive */
	if (owner == self)
	{
		lock->xplk_Count++;
		XPT_MUTEX_UNLOCK(&lock->xplk_Mutex);
		return; /* nesting exclusive lock */
	}
	
	/* request lock */
	request.xplw_Task = self;
	request.xplw_Shared = shared;
	xpi_addtail(&lock->xplk_Waiters, (struct XPMessage *) &request.xplw_Node);
	XPT_MUTEX_UNLOCK(&lock->xplk_Mutex);
	xp_wait(xpbase, XPT_SIG_SYNC);
}

static void xp_lock(struct XPBase *xpbase, struct XPLock *lock)
{
	xp_lock_internal(xpbase, lock, xp_findthread(xpbase, NULL), 0);
}

static void xp_lockshared(struct XPBase *xpbase, struct XPLock *lock)
{
	xp_lock_internal(xpbase, lock, xp_findthread(xpbase, NULL), 1);
}

static void xp_unlock(struct XPBase *xpbase, struct XPLock *lock)
{
	while ((XPT_MUTEX_TRYLOCK(&lock->xplk_Mutex)));
	if (--lock->xplk_Count == 0)
	{
		struct XPLockWait *waiter;
		waiter = (struct XPLockWait *) xpi_remhead(&lock->xplk_Waiters);
		if (waiter)
		{
			if (!waiter->xplw_Shared)
			{
				lock->xplk_Owner = waiter->xplw_Task;
				lock->xplk_Count = 1;
				xp_signal(xpbase, waiter->xplw_Task, XPT_SIG_SYNC);
			}
			else
			{
				struct XPNode *next, *node = (struct XPNode *) lock->xplk_Waiters.xpmq_Head;
				lock->xplk_Owner = NULL;
				for (; (next = node->xpln_Succ); node = next)
				{
					waiter = (struct XPLockWait *) node;
					if (waiter->xplw_Shared)
					{
						xpi_remove((struct XPMessage *) node);
						lock->xplk_Count++;
						xp_signal(xpbase, waiter->xplw_Task, XPT_SIG_SYNC);
					}
				}
			}
		}
		else
			lock->xplk_Owner = NULL;
	}
	XPT_MUTEX_UNLOCK(&lock->xplk_Mutex);
}


static XPTHREADENTRY xpi_threadentry(struct XPThread *xpt)
{
	struct XPBase *xpbase = xpt->xpt_Base;
	struct XPThreadInternal *xpi = xpbase->internal;

	XPT_TLS_SET(xpi->xpi_TLSKey, xpt);

	xp_wait(xpbase, XPT_SIG_SYNC);

	XPT_MUTEX_LOCK(&xpi->xpi_BaseLock);
	xpt->xpt_Status = XPT_RUN;
	XPT_MUTEX_UNLOCK(&xpi->xpi_BaseLock);

	(*xpt->xpt_Function)(xpbase);

	XPT_MUTEX_LOCK(&xpi->xpi_BaseLock);
	xpt->xpt_Status = XPT_DEAD;
	XPT_MUTEX_UNLOCK(&xpi->xpi_BaseLock);

	xp_signal(xpbase, xpt->xpt_Parent, XPT_SIG_SYNC);

	return XPT_THREAD_EXIT();
}

static struct XPThread *xp_createthread(struct XPBase *xpbase,
	void (*userfunc)(struct XPBase *), void *userdata, const char *name)
{
	struct XPThread *xpt = malloc(sizeof(struct XPThread));
	if (xpt)
	{
		size_t nlen = name ? strlen(name) : 0;
		xpt->xpt_Name = malloc(nlen + 1);
		if (xpt->xpt_Name)
		{
			strncpy(xpt->xpt_Name, name, nlen);
			xpt->xpt_Name[nlen] = 0;
			if (XPT_EVENT_INIT(&xpt->xpt_SigEvent))
			{
				if (XPT_MUTEX_INIT(&xpt->xpt_SigLock))
				{
					xpt->xpt_SyncPort = xp_createport(xpbase, xpt,
						XPT_SIG_SYNC);
					xpt->xpt_UserPort = xp_createport(xpbase, xpt,
						XPT_SIG_USER);
					if (xpt->xpt_SyncPort && xpt->xpt_UserPort)
					{
						struct XPThreadInternal *xpi = xpbase->internal;

						xpt->xpt_Base = xpbase;
						xpt->xpt_SigFree = ~XPT_SIG_RESERVED;
						xpt->xpt_SigUsed = 0;
						xpt->xpt_SigState = 0;
						xpt->xpt_Function = userfunc;
						xpt->xpt_UserData = userdata;
						xpt->xpt_Status = XPT_INIT;
						xpt->xpt_RefCount = 0;
						xpt->xpt_Parent = NULL;

						XPT_MUTEX_LOCK(&xpi->xpi_BaseLock);
						xpi_addtail(&xpi->xpi_Tasks, &xpt->xpt_Node);
						XPT_MUTEX_UNLOCK(&xpi->xpi_BaseLock);

						if (userfunc)
						{
							/* create new thread: */
							if (XPT_THREAD_INIT(&xpt->xpt_Thread,
								xpi_threadentry, xpt))
							{
								xpt->xpt_Parent = xp_findthread(xpbase, NULL);
								xp_signal(xpbase, xpt, XPT_SIG_SYNC);
								return xpt;
							}
						}
						else
						{
							/* prepare base thread: */
							XPT_TLS_SET(xpi->xpi_TLSKey, xpt);
							return xpt;
						}
					}
					xp_destroyport(xpbase, xpt->xpt_UserPort);
					xp_destroyport(xpbase, xpt->xpt_SyncPort);
					XPT_MUTEX_DESTROY(&xpt->xpt_SigLock);
				}
				XPT_EVENT_DESTROY(&xpt->xpt_SigEvent);
			}
			free(xpt->xpt_Name);
		}
		free(xpt);
	}
	return NULL;
}

static void xp_destroythread(struct XPBase *xpbase, struct XPThread *xpt)
{
	xp_signal(xpbase, xpt, XPT_SIG_ABORT);
	if (xpt->xpt_Function)
	{
		struct XPThreadInternal *xpi = xpbase->internal;

		for (;;)
		{
			struct XPMessage *msg, *next;
			int found = 0;

			XPT_MUTEX_LOCK(&xpi->xpi_BaseLock);
			msg = xpi->xpi_Tasks.xpmq_Head;
			for (; (next = msg->xpmsg_Succ); msg = next)
			{
				if (msg == &xpt->xpt_Node && xpt->xpt_Status == XPT_DEAD &&
					xpt->xpt_RefCount == 0)
				{
					xpi_remove(msg);
					found = 1;
					break;
				}
			}
			XPT_MUTEX_UNLOCK(&xpi->xpi_BaseLock);

			if (found)
				break;

			xp_wait(xpbase, XPT_SIG_SYNC);
		}
		XPT_THREAD_DESTROY(&xpt->xpt_Thread);
	}
	else
		xpi_remove(&xpt->xpt_Node);

	xp_destroyport(xpbase, xpt->xpt_UserPort);
	xp_destroyport(xpbase, xpt->xpt_SyncPort);
	XPT_MUTEX_DESTROY(&xpt->xpt_SigLock);
	XPT_EVENT_DESTROY(&xpt->xpt_SigEvent);
	free(xpt->xpt_Name);
	free(xpt);
}

EXPORT struct XPBase *xpthread_create(void *args)
{
	struct XPBase *xpbase = malloc(sizeof(struct XPBase));
	struct XPThreadInternal *xpi = malloc(sizeof(struct XPThreadInternal));
	if (xpbase && xpi)
	{
		memset(xpbase, 0, sizeof(*xpbase));
		memset(xpi, 0, sizeof(*xpi));

		xpbase->internal = xpi;
		xpbase->size = sizeof(struct XPBase);
		xpbase->version = VERSION;
		xpbase->revision = REVISION;

		if (XPT_TLS_INIT(&xpi->xpi_TLSKey))
		{
			if (XPT_MUTEX_INIT(&xpi->xpi_BaseLock))
			{
				xpi_initlist(&xpi->xpi_Tasks);
				xpi->xpi_BaseThread = xp_createthread(xpbase, NULL, NULL,
					NULL);
				if (xpi->xpi_BaseThread)
				{
					xpbase->createthread = xp_createthread;
					xpbase->destroythread = xp_destroythread;
					xpbase->findthread = xp_findthread;
					xpbase->getdata = xp_getdata;
					xpbase->setdata = xp_setdata;
					xpbase->signal = xp_signal;
					xpbase->setsignal = xp_setsignal;
					xpbase->wait = xp_wait;
					xpbase->timedwait = xp_timedwait;
					xpbase->getuserport = xp_getuserport;
					xpbase->getportsignal = xp_getportsignal;
					xpbase->putmsg = xp_putmsg;
					xpbase->getmsg = xp_getmsg;
					xpbase->replymsg = xp_replymsg;
					xpbase->gettime = xp_gettime;
					xpbase->subtime = xp_subtime;
					xpbase->addtime = xp_addtime;
					xpbase->cmptime = xp_cmptime;
					xpbase->renamethread = xp_renamethread;
					xpbase->getname = xp_getname;
					xpbase->refthread = xp_refthread;
					xpbase->unrefthread = xp_unrefthread;
					xpbase->createfastmutex = xp_createfastmutex;
					xpbase->destroyfastmutex = xp_destroyfastmutex;
					xpbase->lockfastmutex = xp_lockfastmutex;
					xpbase->trylockfastmutex = xp_trylockfastmutex;
					xpbase->unlockfastmutex = xp_unlockfastmutex;
					xpbase->createlock = xp_createlock;
					xpbase->destroylock = xp_destroylock;
					xpbase->lock = xp_lock;
					xpbase->lockshared = xp_lockshared;
					xpbase->unlock = xp_unlock;
					
					return xpbase;
				}
				XPT_MUTEX_DESTROY(&xpi->xpi_BaseLock);
			}
			XPT_TLS_DESTROY(&xpi->xpi_TLSKey);
	  }
	}
	free(xpi);
	free(xpbase);
	return NULL;
}

EXPORT void xpthread_destroy(struct XPBase *xpbase)
{
	struct XPThreadInternal *xpi = xpbase->internal;
	xp_destroythread(xpbase, xpi->xpi_BaseThread);
	assert(ISLISTEMPTY(&xpi->xpi_Tasks));
	XPT_MUTEX_DESTROY(&xpi->xpi_BaseLock);
	XPT_TLS_DESTROY(&xpi->xpi_TLSKey);
	free(xpi);
	free(xpbase);
}
