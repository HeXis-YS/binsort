#ifndef _XPTHREAD_H
#define _XPTHREAD_H

/*
**	xpthread 2.0 - cross-platform multithreading library
**	Copyright (c) 2003-2011 Timm S. Mueller, all rights reserved
**	Licensed under the 3-clause BSD license, see COPYRIGHT
**
**	xpbase = xpthread_create(args)
**		Creates a xpthread base handle that is anchored to the caller's
**		context. Returns the handle, or NULL if initialization failed.
**		The handle should be destroyed with a call to xpthread_destroy().
**		Args is reserved for future extensions and must be NULL.
**
**	xpthread_destroy(xpbase)
**		Destroys the xpthread handle and frees all associated resources.
**
**	thread = (*xpbase->createthread)(xpbase, function, userdata, taskname)
**		Creates a new thread and optionally attaches a name and some userdata
**		with it. If successful, the specified function will be running in a
**		new thread immediately. Returns a thread handle, or NULL if something
**		went wrong. The handle returned must be destroyed with a call to
**		xpbase->destroythread() when the thread is no longer needed (see
**		annotations below).
**
**	(*xpbase->destroythread)(xpbase, thread)
**		Destroys the specified thread. This function will block until the
**		thread has concluded and exited properly. Threads are never killed
**		or forcefully aborted - always leave them gently through their
**		function exit.
**
**	thread = (*xpbase->findthread)(xpbase, name)
**		Finds a thread by its name or, if name is NULL, returns the handle
**		of the thread the caller is running in.
**
**	userdata = (*xpbase->getdata)(xpbase, thread)
**		Gets the userdata pointer associated with the specified thread.
**
**	old_userdata = (*xpbase->setdata)(xpbase, userdata)
**		Attaches a new userdata pointer to the addressed thread. The old
**		userdata pointer is returned.
**
**	sigmask = (*xpbase->allocsignal)(xpbase, thread, signals)
**		Allocates one or more signals from a thread; if sigs is 0, any free
**		signal will be allocated, otherwise the exact set specified. Returns
**		the allocated signals, or 0 if no more free signals (or not all of
**		the signals in the set) were available.
**
**	(*xpbase->freesignal)(xpbase, thread, signals)
**		Returns the specified signals to the thread's pool of free signals.
**
**	(*xpbase->signal)(xpbase, thread, signals)
**		Sends the specified signals to a thread, causing the thread to
**		wake up if it was suspended waiting for one of these signals.
**
**	old_signals = (*xpbase->setsignal)(xpbase, new_signals, sigmask)
**		Sets the specified signals in the caller's thread, masked through
**		the sigmask argument, and returns the old state of all signals.
**
**	signals = (*xpbase->wait)(xpbase, signals)
**		Blocks the caller's thread and waits for any of the specified signals
**		to appear. Returns immediately if any of the signals were already
**		pending. The affecting signals will be cleared from the thread's
**		signal state before returning to the caller. Note: If none of these
**		signals show up, this function will block forever.
**
**	signals = (*xpbase->timedwait)(xpbase, signals, timeout)
**		The same as xpbase->wait() with an additional (absolute) timeout
**		that is determined by a timeout structure and its fields xptm_Sec
**		(seconds) and xptm_USec (microseconds). The return value will be 0
**		if a timeout caused this function to return. An absolute time is
**		measured in the number of seconds elapsed since 1-1-1970.
**
**	port = (*xpbase->createport)(xpbase, thread, prefsignal)
**		Creates a new message port (aka "mailbox"), which will belong to and
**		operate in the addressed thread. If prefsignal is 0, this function
**		attempts to reserve a free signal for use by the message port -
**		otherwise, it tries to bind the message port to the signal specified.
**		When a message arrives at the port, it will cause this signal to show
**		up in the thread owning it, possibly waking it up when it was
**		suspended.
**
**	(*xpbase->destroyport)(xpbase, port)
**		Destroys the specified message port and frees the underlying signal
**		in the thread owning it.
**
**	signal = (*xpbase->getportsignal)(xpbase, port)
**		Returns the specified port's underlying signal, which can be used
**		in xpbase->wait() for synchronization on the arrival of messages.
**
**	(*xpbase->putmsg)(xpbase, port, replyport, message)
**		Puts a message to the specified message port, possibly waking up the
**		thread that was suspended waiting for the port's underlying signal.
**		An optional replyport can be attached to the message, which will
**		be used for sending the message back when the receveiver replies
**		the message with xpbase->replymsg(). For one-way messages, use NULL
**		for the replyport argument; this will cause xpbase->replymsg() to
**		free() the message transparently.
**
**	message = (*xpbase->getmsg)(xpbase, port)
**		Returns the next pending message from the specified message port,
**		or NULL if the message port was empty.
**
**	(*xpbase->replymsg)(xpbase, message)
**		Replies the message to the message port that was specified as the
**		replyport when it was sent using xpbase->putmsg(), or frees the
**		message transparently (see putmsg).
**
**	(*xpbase->gettime)(time)
**		Places the current absolute system time in the XPTime structure.
**		An absolute time is measured in the number of seconds elapsed since
**		1-1-1970.
**
**	xpbase->subtime(timea, timeb)
**		Subtracts the time in XPTime structure b from time in a.
**
**	xpbase->addtime(timea, timeb)
**		Adds the time XPTime structure b to the time in a.
**
**	xpbase->cmptime(a, b)
**		Compares time a with time b. The result will be 1 if time a refers
**		to a later point in time than time b, -1 if time a refers to a
**		earlier point in time than time b, and 0 if time a is equal time b.
*/

/*****************************************************************************/

#if defined(XPT_PTHREADS)

#include <stdlib.h>
#include <stdint.h>
typedef uint32_t XPUINT32;
typedef int32_t XPINT32;

#elif defined(XPT_WINDOWS)

#include <windows.h>
typedef DWORD XPUINT32;
typedef LONG XPINT32;

#else

#error platform not supported

#endif

/* guaranteed 32bit unsigned integer: */
typedef XPUINT32 XPSIGMASK;

/* forward declaration: */
struct XPThread;
struct XPPort;
struct XPFastMutex;
struct XPLock;

/* Message header; implements a minimal node: */
struct XPMessage
{
	struct XPMessage *xpmsg_Succ;
	struct XPMessage *xpmsg_Pred;
	struct XPPort *xpmsg_ReplyPort;
};

/* Time structure: */
struct XPTime
{
	XPINT32 xptm_Sec;
	XPINT32 xptm_USec;
};

/* Pre-defined signals with reserved meaning: */
#define XPT_SIG_SYNC		0x00000001
#define XPT_SIG_USER		0x00000002
#define XPT_SIG_ABORT		0x00000004

/* Mask of predefined (non-allocatable) signals: */
#define XPT_SIG_RESERVED	0x0000ffff

/* XPThread interface: */
struct XPBase
{
	size_t size;
	short version, revision;
	void *internal;
	
	/* create, destroy: */
	struct XPThread *(*createthread)(struct XPBase *, void (*function)(struct XPBase *), void *udata, const char *taskname);
	void (*destroythread)(struct XPBase *, struct XPThread *thread);
	
	/* thread names: */
	struct XPThread *(*findthread)(struct XPBase *, const char *name);
	int (*renamethread)(struct XPBase *xpbase, struct XPThread *xpt, const char *newname);
	size_t (*getname)(struct XPBase *xpbase, struct XPThread *xpt, char *namebuf, size_t namebuf_len);
	
	/* set/get userdata: */
	void *(*getdata)(struct XPBase *, struct XPThread *thread);
	void *(*setdata)(struct XPBase *, struct XPThread *thread, void *data);
	
	/* signalling: */
	void (*signal)(struct XPBase *, struct XPThread *, XPSIGMASK signals);
	XPSIGMASK (*setsignal)(struct XPBase *, XPSIGMASK newsig, XPSIGMASK sigmask);
	XPSIGMASK (*wait)(struct XPBase *, XPSIGMASK sigmask);
	XPSIGMASK (*timedwait)(struct XPBase *, XPSIGMASK sigmask, const struct XPTime *time);
	
	/* message ports (a.ka. mailboxes), every thread has one: */
	struct XPPort *(*getuserport)(struct XPBase *, struct XPThread *);
	XPSIGMASK (*getportsignal)(struct XPBase *, struct XPPort *);
	void (*putmsg)(struct XPBase *, struct XPPort *port, struct XPPort *replyport, struct XPMessage *);
	struct XPMessage *(*getmsg)(struct XPBase *, struct XPPort *port);
	void (*replymsg)(struct XPBase *, struct XPMessage *);
	
	/* timing functions: */
	void (*gettime)(struct XPBase *xpbase, struct XPTime *time);
	void (*subtime)(struct XPBase *xpbase, struct XPTime *a, const struct XPTime *b);
	void (*addtime)(struct XPBase *xpbase, struct XPTime *a, const struct XPTime *b);
	int (*cmptime)(struct XPBase *xpbase, const struct XPTime *a, const struct XPTime *b);
	
	/* intentionally undocumented (needed somewhere, but I forgot where) */
	struct XPThread *(*refthread)(struct XPBase *xpbase, const char *name);
	void (*unrefthread)(struct XPBase *xpbase, struct XPThread *xpt);
	
	/* fast mutexes are as fast as possible, but may be busy looping */
	struct XPFastMutex *(*createfastmutex)(struct XPBase *xpbase);
	void (*destroyfastmutex)(struct XPBase *xpbase, struct XPFastMutex *mutex);
	void (*lockfastmutex)(struct XPBase *xpbase, struct XPFastMutex *mutex);
	int (*trylockfastmutex)(struct XPBase *xpbase, struct XPFastMutex *mutex);
	void (*unlockfastmutex)(struct XPBase *xpbase, struct XPFastMutex *mutex);

#if 0
	/* "locks" are recursive and support r/o and r/w accesses (not tested): */
	struct XPLock *(*createlock)(struct XPBase *xpbase);
	void (*destroylock)(struct XPBase *xpbase, struct XPLock *);
	void (*lock)(struct XPBase *xpbase, struct XPLock *);
	void (*lockshared)(struct XPBase *xpbase, struct XPLock *);
	void (*unlock)(struct XPBase *xpbase, struct XPLock *);
#endif
};

extern struct XPBase *xpthread_create(void *args);
extern void xpthread_destroy(struct XPBase *xpbase);
extern struct XPBase *xpthread_create_once(void *args);

#endif /* _XPTHREAD_H */
