/* wtp.c
 *
 * This file implements the worker thread pool (wtp) class.
 * 
 * File begun on 2008-01-20 by RGerhards
 *
 * There is some in-depth documentation available in doc/dev_queue.html
 * (and in the web doc set on http://www.rsyslog.com/doc). Be sure to read it
 * if you are getting aquainted to the object.
 *
 * Copyright 2008,2009 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of the rsyslog runtime library.
 *
 * The rsyslog runtime library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The rsyslog runtime library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the rsyslog runtime library.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 * A copy of the LGPL can be found in the file "COPYING.LESSER" in this distribution.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <atomic.h>
#if HAVE_SYS_PRCTL_H
#  include <sys/prctl.h>
#endif

/// TODO: check on solaris if this is any longer needed - I don't think so - rgerhards, 2009-09-20
//#ifdef OS_SOLARIS
//#	include <sched.h>
//#endif

#include "rsyslog.h"
#include "stringbuf.h"
#include "srUtils.h"
#include "wtp.h"
#include "wti.h"
#include "obj.h"
#include "unicode-helper.h"
#include "glbl.h"

/* static data */
DEFobjStaticHelpers
DEFobjCurrIf(glbl)

/* forward-definitions */

/* methods */

/* get the header for debug messages
 * The caller must NOT free or otherwise modify the returned string!
 */
static inline uchar *
wtpGetDbgHdr(wtp_t *pThis)
{
	ISOBJ_TYPE_assert(pThis, wtp);

	if(pThis->pszDbgHdr == NULL)
		return (uchar*) "wtp"; /* should not normally happen */
	else
		return pThis->pszDbgHdr;
}



/* Not implemented dummy function for constructor */
static rsRetVal NotImplementedDummy() { return RS_RET_NOT_IMPLEMENTED; }
/* Standard-Constructor for the wtp object
 */
BEGINobjConstruct(wtp) /* be sure to specify the object type also in END macro! */
	pthread_mutex_init(&pThis->mutWtp, NULL);
	pthread_cond_init(&pThis->condThrdTrm, NULL);
	/* set all function pointers to "not implemented" dummy so that we can safely call them */
	pThis->pfChkStopWrkr = NotImplementedDummy;
	pThis->pfGetDeqBatchSize = NotImplementedDummy;
	pThis->pfIsIdle = NotImplementedDummy;
	pThis->pfDoWork = NotImplementedDummy;
	pThis->pfObjProcessed = NotImplementedDummy;
	pThis->pfOnIdle = NotImplementedDummy;
	pThis->pfOnWorkerCancel = NotImplementedDummy;
	pThis->pfOnWorkerStartup = NotImplementedDummy;
	pThis->pfOnWorkerShutdown = NotImplementedDummy;
ENDobjConstruct(wtp)


/* Construction finalizer
 * rgerhards, 2008-01-17
 */
rsRetVal
wtpConstructFinalize(wtp_t *pThis)
{
	DEFiRet;
	int i;
	uchar pszBuf[64];
	size_t lenBuf;
	wti_t *pWti;

	ISOBJ_TYPE_assert(pThis, wtp);

	DBGPRINTF("%s: finalizing construction of worker thread pool\n", wtpGetDbgHdr(pThis));
	/* alloc and construct workers - this can only be done in finalizer as we previously do
	 * not know the max number of workers
	 */
	if((pThis->pWrkr = malloc(sizeof(wti_t*) * pThis->iNumWorkerThreads)) == NULL)
		ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);
	
	for(i = 0 ; i < pThis->iNumWorkerThreads ; ++i) {
		CHKiRet(wtiConstruct(&pThis->pWrkr[i]));
		pWti = pThis->pWrkr[i];
		lenBuf = snprintf((char*)pszBuf, sizeof(pszBuf), "%s/w%d", wtpGetDbgHdr(pThis), i);
		CHKiRet(wtiSetDbgHdr(pWti, pszBuf, lenBuf));
		CHKiRet(wtiSetpWtp(pWti, pThis));
		CHKiRet(wtiConstructFinalize(pWti));
	}
		

finalize_it:
	RETiRet;
}


/* Destructor */
BEGINobjDestruct(wtp) /* be sure to specify the object type also in END and CODESTART macros! */
	int i;
CODESTARTobjDestruct(wtp)
	/* destruct workers */
	for(i = 0 ; i < pThis->iNumWorkerThreads ; ++i)
		wtiDestruct(&pThis->pWrkr[i]);

	free(pThis->pWrkr);
	pThis->pWrkr = NULL;

	/* actual destruction */
	pthread_cond_destroy(&pThis->condThrdTrm);
	pthread_mutex_destroy(&pThis->mutWtp);

	free(pThis->pszDbgHdr);
ENDobjDestruct(wtp)


/* wake up all worker threads.
 * rgerhards, 2008-01-16
 */
rsRetVal
wtpWakeupAllWrkr(wtp_t *pThis)
{
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, wtp);
	d_pthread_mutex_lock(pThis->pmutUsr);
	pthread_cond_broadcast(pThis->pcondBusy);
	d_pthread_mutex_unlock(pThis->pmutUsr);
	RETiRet;
}


/* Sent a specific state for the worker thread pool. -- rgerhards, 2008-01-21
 * We do not need to do atomic instructions as set operations are only
 * called when terminating the pool, and then in strict sequence. So we
 * can never overwrite each other. On the other hand, it also doesn't
 * matter if the read operation obtains an older value, as we then simply
 * do one more iteration, what is perfectly legal (during shutdown
 * they are awoken in any case). -- rgerhards, 2009-07-20
 */
rsRetVal
wtpSetState(wtp_t *pThis, wtpState_t iNewState)
{
	ISOBJ_TYPE_assert(pThis, wtp);
	pThis->wtpState = iNewState;
	return RS_RET_OK;
}


/* check if the worker shall shutdown (1 = yes, 0 = no)
 * Note: there may be two mutexes locked, the bLockUsrMutex is the one in our "user"
 * (e.g. the queue clas)
 * rgerhards, 2008-01-21
 */
rsRetVal
wtpChkStopWrkr(wtp_t *pThis, int bLockUsrMutex)
{
	DEFiRet;
	wtpState_t wtpState;

	ISOBJ_TYPE_assert(pThis, wtp);
	/* we need a consistent value, but it doesn't really matter if it is changed
	 * right after the fetch - then we simply do one more iteration in the worker
	 */
	wtpState = ATOMIC_FETCH_32BIT(pThis->wtpState);

	if(wtpState == wtpState_SHUTDOWN_IMMEDIATE) {
		ABORT_FINALIZE(RS_RET_TERMINATE_NOW);
	} else if(wtpState == wtpState_SHUTDOWN) {
		ABORT_FINALIZE(RS_RET_TERMINATE_WHEN_IDLE);
	}

	/* try customer handler if one was set and we do not yet have a definite result */
	if(pThis->pfChkStopWrkr != NULL) {
		iRet = pThis->pfChkStopWrkr(pThis->pUsr, bLockUsrMutex);
	}

finalize_it:
	RETiRet;
}


#pragma GCC diagnostic ignored "-Wempty-body"
/* Send a shutdown command to all workers and see if they terminate.
 * A timeout may be specified. This function may also be called with
 * the current number of workers being 0, in which case it does not
 * shut down any worker.
 * rgerhards, 2008-01-14
 */
rsRetVal
wtpShutdownAll(wtp_t *pThis, wtpState_t tShutdownCmd, struct timespec *ptTimeout)
{
	DEFiRet;
	int bTimedOut;

	ISOBJ_TYPE_assert(pThis, wtp);

	wtpSetState(pThis, tShutdownCmd);
	wtpWakeupAllWrkr(pThis);

	/* wait for worker thread termination */
	d_pthread_mutex_lock(&pThis->mutWtp);
	pthread_cleanup_push(mutexCancelCleanup, &pThis->mutWtp);
	bTimedOut = 0;
	while(pThis->iCurNumWrkThrd > 0 && !bTimedOut) {
		DBGPRINTF("%s: waiting %ldms on worker thread termination, %d still running\n",
			   wtpGetDbgHdr(pThis), timeoutVal(ptTimeout), ATOMIC_FETCH_32BIT(pThis->iCurNumWrkThrd));

		if(d_pthread_cond_timedwait(&pThis->condThrdTrm, &pThis->mutWtp, ptTimeout) != 0) {
			DBGPRINTF("%s: timeout waiting on worker thread termination\n", wtpGetDbgHdr(pThis));
			bTimedOut = 1;	/* we exit the loop on timeout */
		}
	}
	pthread_cleanup_pop(1);

	if(bTimedOut)
		iRet = RS_RET_TIMED_OUT;
	
	RETiRet;
}
#pragma GCC diagnostic warning "-Wempty-body"


/* Unconditionally cancel all running worker threads.
 * rgerhards, 2008-01-14
 */
rsRetVal
wtpCancelAll(wtp_t *pThis)
{
	DEFiRet;
	int i;

	ISOBJ_TYPE_assert(pThis, wtp);

	/* go through all workers and cancel those that are active */
	for(i = 0 ; i < pThis->iNumWorkerThreads ; ++i) {
		wtiCancelThrd(pThis->pWrkr[i]);
	}

	RETiRet;
}


/* cancellation cleanup handler for executing worker
 * decrements the worker counter
 * rgerhards, 2008-01-20
 */
static void
wtpWrkrExecCancelCleanup(void *arg)
{
	wti_t *pWti = (wti_t*) arg;
	wtp_t *pThis;

	BEGINfunc
	ISOBJ_TYPE_assert(pWti, wti);
	pThis = pWti->pWtp;
	ISOBJ_TYPE_assert(pThis, wtp);

	wtiSetState(pWti, WRKTHRD_STOPPED);
	ATOMIC_DEC(pThis->iCurNumWrkThrd);
	pthread_cond_broadcast(&pThis->condThrdTrm); /* activate anyone waiting on thread shutdown */

	DBGPRINTF("%s: Worker thread %lx, terminated, num workers now %d\n",
		  wtpGetDbgHdr(pThis), (unsigned long) pWti, ATOMIC_FETCH_32BIT(pThis->iCurNumWrkThrd));
	ENDfunc
}


/* wtp worker shell. This is started and calls into the actual
 * wti worker.
 * rgerhards, 2008-01-21
 */
#pragma GCC diagnostic ignored "-Wempty-body"
static void *
wtpWorker(void *arg) /* the arg is actually a wti object, even though we are in wtp! */
{
	uchar *pszDbgHdr;
	uchar thrdName[32] = "rs:";
	wti_t *pWti = (wti_t*) arg;
	wtp_t *pThis;
	sigset_t sigSet;

	BEGINfunc
	ISOBJ_TYPE_assert(pWti, wti);
	pThis = pWti->pWtp;
	ISOBJ_TYPE_assert(pThis, wtp);

	sigfillset(&sigSet);
	pthread_sigmask(SIG_BLOCK, &sigSet, NULL);

#	if HAVE_PRCTL && defined PR_SET_NAME
	/* set thread name - we ignore if the call fails, has no harsh consequences... */
	pszDbgHdr = wtpGetDbgHdr(pThis);
	ustrncpy(thrdName+3, pszDbgHdr, 20);
	if(prctl(PR_SET_NAME, thrdName, 0, 0, 0) != 0) {
		DBGPRINTF("prctl failed, not setting thread name for '%s'\n", wtpGetDbgHdr(pThis));
	}
#	endif

	pthread_cleanup_push(wtpWrkrExecCancelCleanup, pWti);
	wtiWorker(pWti);
	pthread_cleanup_pop(1);

	ENDfunc
	pthread_exit(0);
}
#pragma GCC diagnostic warning "-Wempty-body"


/* start a new worker */
static rsRetVal
wtpStartWrkr(wtp_t *pThis, int bLockMutex)
{
	DEFVARS_mutexProtection;
	wti_t *pWti;
	int i;
	int iState;
	pthread_attr_t attr;
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, wtp);

	BEGIN_MTX_PROTECTED_OPERATIONS(&pThis->mutWtp, bLockMutex);

	/* find free spot in thread table. */
	for(i = 0 ; i < pThis->iNumWorkerThreads ; ++i) {
		if(wtiGetState(pThis->pWrkr[i]) == WRKTHRD_STOPPED) {
			break;
		}
	}

	if(i == pThis->iNumWorkerThreads)
		ABORT_FINALIZE(RS_RET_NO_MORE_THREADS);

	if(i == 0 || pThis->toWrkShutdown == -1) {
		wtiSetAlwaysRunning(pThis->pWrkr[i]);
	}

	pWti = pThis->pWrkr[i];
	wtiSetState(pWti, WRKTHRD_RUNNING);
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	iState = pthread_create(&(pWti->thrdID), &attr, wtpWorker, (void*) pWti);
	pthread_attr_destroy(&attr);	/* TODO: we could globally reuse such an attribute 2009-07-08 */
	ATOMIC_INC(pThis->iCurNumWrkThrd); /* we got one more! */

	DBGPRINTF("%s: started with state %d, num workers now %d\n",
		  wtpGetDbgHdr(pThis), iState, ATOMIC_FETCH_32BIT(pThis->iCurNumWrkThrd));

finalize_it:
	END_MTX_PROTECTED_OPERATIONS(&pThis->mutWtp);
	RETiRet;
}


/* set the number of worker threads that should be running. If less than currently running,
 * a new worker may be started. Please note that there is no guarantee the number of workers
 * said will be running after we exit this function. It is just a hint. If the number is
 * higher than one, and no worker is started, the "busy" condition is signaled to awake a worker.
 * So the caller can assume that there is at least one worker re-checking if there is "work to do"
 * after this function call.
 * rgerhards, 2008-01-21
 */
rsRetVal
wtpAdviseMaxWorkers(wtp_t *pThis, int nMaxWrkr)
{
	DEFiRet;
	int nMissing; /* number workers missing to run */
	int i;

	ISOBJ_TYPE_assert(pThis, wtp);

	if(nMaxWrkr == 0)
		FINALIZE;

	if(nMaxWrkr > pThis->iNumWorkerThreads) /* limit to configured maximum */
		nMaxWrkr = pThis->iNumWorkerThreads;

	nMissing = nMaxWrkr - ATOMIC_FETCH_32BIT(pThis->iCurNumWrkThrd);

	if(nMissing > 0) {
		DBGPRINTF("%s: high activity - starting %d additional worker thread(s).\n", wtpGetDbgHdr(pThis), nMissing);
		/* start the rqtd nbr of workers */
		for(i = 0 ; i < nMissing ; ++i) {
			CHKiRet(wtpStartWrkr(pThis, LOCK_MUTEX));
		}
	} else {
		pthread_cond_signal(pThis->pcondBusy);
	}

	
finalize_it:
	RETiRet;
}


/* some simple object access methods */
DEFpropSetMeth(wtp, toWrkShutdown, long)
DEFpropSetMeth(wtp, wtpState, wtpState_t)
DEFpropSetMeth(wtp, iNumWorkerThreads, int)
DEFpropSetMeth(wtp, pUsr, void*)
DEFpropSetMethPTR(wtp, pmutUsr, pthread_mutex_t)
DEFpropSetMethPTR(wtp, pcondBusy, pthread_cond_t)
DEFpropSetMethFP(wtp, pfChkStopWrkr, rsRetVal(*pVal)(void*, int))
DEFpropSetMethFP(wtp, pfRateLimiter, rsRetVal(*pVal)(void*))
DEFpropSetMethFP(wtp, pfGetDeqBatchSize, rsRetVal(*pVal)(void*, int*))
DEFpropSetMethFP(wtp, pfIsIdle, rsRetVal(*pVal)(void*, wtp_t*))
DEFpropSetMethFP(wtp, pfDoWork, rsRetVal(*pVal)(void*, void*))
DEFpropSetMethFP(wtp, pfObjProcessed, rsRetVal(*pVal)(void*, wti_t*))
DEFpropSetMethFP(wtp, pfOnIdle, rsRetVal(*pVal)(void*, int))
DEFpropSetMethFP(wtp, pfOnWorkerCancel, rsRetVal(*pVal)(void*, void*))
DEFpropSetMethFP(wtp, pfOnWorkerStartup, rsRetVal(*pVal)(void*))
DEFpropSetMethFP(wtp, pfOnWorkerShutdown, rsRetVal(*pVal)(void*))


/* set the debug header message
 * The passed-in string is duplicated. So if the caller does not need
 * it any longer, it must free it. Must be called only before object is finalized.
 * rgerhards, 2008-01-09
 */
rsRetVal
wtpSetDbgHdr(wtp_t *pThis, uchar *pszMsg, size_t lenMsg)
{
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, wtp);
	assert(pszMsg != NULL);
	
	if(lenMsg < 1)
		ABORT_FINALIZE(RS_RET_PARAM_ERROR);

	if(pThis->pszDbgHdr != NULL) {
		free(pThis->pszDbgHdr);
		pThis->pszDbgHdr = NULL;
	}

	if((pThis->pszDbgHdr = malloc(sizeof(uchar) * lenMsg + 1)) == NULL)
		ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);

	memcpy(pThis->pszDbgHdr, pszMsg, lenMsg + 1); /* always think about the \0! */

finalize_it:
	RETiRet;
}

/* dummy */
rsRetVal wtpQueryInterface(void) { return RS_RET_NOT_IMPLEMENTED; }

/* exit our class
 */
BEGINObjClassExit(wtp, OBJ_IS_CORE_MODULE) /* CHANGE class also in END MACRO! */
CODESTARTObjClassExit(nsdsel_gtls)
	/* release objects we no longer need */
	objRelease(glbl, CORE_COMPONENT);
ENDObjClassExit(wtp)


/* Initialize the stream class. Must be called as the very first method
 * before anything else is called inside this class.
 * rgerhards, 2008-01-09
 */
BEGINObjClassInit(wtp, 1, OBJ_IS_CORE_MODULE)
	/* request objects we use */
	CHKiRet(objUse(glbl, CORE_COMPONENT));
ENDObjClassInit(wtp)

/* vi:set ai:
 */
