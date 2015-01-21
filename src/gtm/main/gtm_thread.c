/*-------------------------------------------------------------------------
 *
 * gtm_thread.c
 *	Thread handling
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#include <pthread.h>
#include "gtm/gtm.h"
#include "gtm/memutils.h"
#include "gtm/gtm_txn.h"
#include "gtm/libpq.h"

static void *GTM_ThreadMainWrapper(void *argp);
static void GTM_ThreadCleanup(void *argp);

GTM_Threads	GTMThreadsData;
GTM_Threads *GTMThreads = &GTMThreadsData;

#define GTM_MIN_THREADS 32			/* Provision for minimum threads */
#define GTM_MAX_THREADS 1024		/* Max threads allowed in the GTM */
#define GTMThreadsFull	(GTMThreads->gt_thread_count == GTMThreads->gt_array_size)

/*
 * Add the given thrinfo structure to the global array, expanding it if
 * necessary
 */
int
GTM_ThreadAdd(GTM_ThreadInfo *thrinfo)
{
	int ii;

	GTM_RWLockAcquire(&GTMThreads->gt_lock, GTM_LOCKMODE_WRITE);

	if (GTMThreadsFull)
	{
		uint32 newsize;

		/*
		 * TODO Optimize lock management by not holding any locks during memory
		 * allocation.
		 */
		if (GTMThreads->gt_array_size == GTM_MAX_THREADS)
		{
			elog(LOG, "Too many threads active");
			GTM_RWLockRelease(&GTMThreads->gt_lock);
			return -1;
		}

		if (GTMThreads->gt_array_size == 0)
			newsize = GTM_MIN_THREADS;
		else
		{
			/*
			 * We ran out of the array size. Just double the size, bound by the
			 * upper limit
			 */
			newsize = GTMThreads->gt_array_size * 2;
		}

		/* Can't have more than GTM_MAX_THREADS */
		if (newsize > GTM_MAX_THREADS)
			newsize = GTM_MAX_THREADS;

		if (GTMThreads->gt_threads == NULL)
			GTMThreads->gt_threads = (GTM_ThreadInfo **)palloc0(sizeof (GTM_ThreadInfo *) * newsize);
		else
		{
			void *old_ptr = GTMThreads->gt_threads;
			GTMThreads->gt_threads = (GTM_ThreadInfo **)palloc0(sizeof (GTM_ThreadInfo *) * newsize);
			memcpy(GTMThreads->gt_threads, old_ptr,
					GTMThreads->gt_array_size * sizeof (GTM_ThreadInfo *));
			pfree(old_ptr);
		}

		GTMThreads->gt_array_size = newsize;
	}

	/*
	 * Now that we have free entries in the array, find a free slot and add the
	 * thrinfo pointer to it.
	 *
	 * TODO Optimize this later by tracking few free slots and reusing them.
	 * The free slots can be updated when a thread exits and reused when a new
	 * thread is added to the pool.
	 */
	for (ii = 0; ii < GTMThreads->gt_array_size; ii++)
	{
		if (GTMThreads->gt_threads[ii] == NULL)
		{
			GTMThreads->gt_threads[ii] = thrinfo;
			GTMThreads->gt_thread_count++;
			break;
		}
	}
	
	/*
	 * Lastly, assign a unique, monotonically increasing identifier to the
	 * remote client. This is sent back to the client and client will resend it
	 * in case of reconnect
	 *
	 * Since all open transactions are tracked in a single linked list on the
	 * GTM, we need a mechanism to identify transactions associated with a
	 * specific client connection so that they can be removed if the client
	 * disconnects abrubptly. We could have something like a pthread_id given
	 * that there is one GTM thread per connection, but that is not sufficient
	 * when GTM is failed over to a standby. The pthread_id on the old master
	 * will make no sense on the new master and it will be hard to re-establish
	 * the association of open transactions and the client connections (note
	 * that all of this applies only when backends are connecting to GTM via a
	 * GTM proxy. Otherwise those open transactions will be aborted when GTM
	 * failover happens)
	 *
	 * So we use a unique identifier for each incoming connection to the GTM.
	 * GTM assigns the identifier and also sends it back to the client as part
	 * of the connection establishment process. In case of GTM failover, and
	 * when GTM proxies reconnect to the new master, they also send back the
	 * identifier issued to them by the previous master. The new GTM master
	 * then uses that identifier to associate open transactions with the client
	 * connection. Of course, for this to work, GTM must store client
	 * identifier in each transaction info structure and also replicate that
	 * information to the standby when new transactions are backed up.
	 *
	 * Since GTM does not backup the action of assinging new identifiers, at
	 * failover, it may happen that the new master hasn't yet seen an
	 * identifier which is already assigned my the old master (say because the
	 * client has not started any transaction yet). To handle this case, we
	 * track the latest identifier as seen my the new master upon failover. If
	 * a client sends an identifier which is newer than that, that identifier
	 * is discarded and new master will issue a new identifier that client will
	 * accept.
	 */
	thrinfo->thr_client_id = GTMThreads->gt_next_client_id;
	GTMThreads->gt_next_client_id = GTM_CLIENT_ID_NEXT(GTMThreads->gt_next_client_id);
	GTM_RWLockRelease(&GTMThreads->gt_lock);

	/*
	 * Track the slot information in the thrinfo. This is useful to quickly
	 * find the slot given the thrinfo structure.
	 */
	thrinfo->thr_localid = ii;
	return ii;
}

int
GTM_ThreadRemove(GTM_ThreadInfo *thrinfo)
{
	int ii;
	GTM_RWLockAcquire(&GTMThreads->gt_lock, GTM_LOCKMODE_WRITE);

	for (ii = 0; ii < GTMThreads->gt_array_size; ii++)
	{
		if (GTMThreads->gt_threads[ii] == thrinfo)
			break;
	}

	if (ii == GTMThreads->gt_array_size)
	{
		elog(LOG, "Thread (%p) not found ", thrinfo);
		GTM_RWLockRelease(&GTMThreads->gt_lock);
		return -1;
	}

	GTMThreads->gt_threads[ii] = NULL;
	GTMThreads->gt_thread_count--;
	GTM_RWLockRelease(&GTMThreads->gt_lock);

	pfree(thrinfo);

	return 0;
}

/*
 * Create a new thread and assign the given connection to it.
 *
 * This function is responsible for setting up the various memory contextes for
 * the thread as well as registering this thread with the Thread Manager.
 *
 * Upon successful creation, the thread will start running the given
 * "startroutine". The thread information is returned to the calling process.
 */
GTM_ThreadInfo *
GTM_ThreadCreate(GTM_ConnectionInfo *conninfo,
				  void *(* startroutine)(void *))
{
	GTM_ThreadInfo *thrinfo;
	int err;

	/*
	 * We are still running in the context of the main thread. So the
	 * allocation below would last as long as the main thread exists or the
	 * memory is explicitely freed.
	 */
	thrinfo = (GTM_ThreadInfo *)palloc0(sizeof (GTM_ThreadInfo));

	thrinfo->thr_conn = conninfo;
	GTM_RWLockInit(&thrinfo->thr_lock);

	/*
	 * The thread status is set to GTM_THREAD_STARTING and will be changed by
	 * the thread itself when it actually starts executing
	 */
	thrinfo->thr_status = GTM_THREAD_STARTING;

	/*
	 * Install the ThreadInfo structure in the global array. We do this before
	 * starting the thread
	 */
	if (GTM_ThreadAdd(thrinfo) == -1)
	{
		GTM_RWLockDestroy(&thrinfo->thr_lock);
		pfree(thrinfo);
		return NULL;
	}

	/*
	 * Set up memory contextes before actually starting the threads
	 *
	 * The TopThreadContext is a child of TopMemoryContext and it will last as
	 * long as the main process or this thread lives
	 *
	 * Thread context is not shared between other threads
	 */
	thrinfo->thr_thread_context = AllocSetContextCreate(TopMemoryContext,
														"TopMemoryContext",
														ALLOCSET_DEFAULT_MINSIZE,
														ALLOCSET_DEFAULT_INITSIZE,
														ALLOCSET_DEFAULT_MAXSIZE,
														false);

	/*
	 * Since the thread is not yes started, TopMemoryContext still points to
	 * the context of the calling thread
	 */
	thrinfo->thr_parent_context = TopMemoryContext;

	/*
	 * Each thread gets its own ErrorContext and its a child of ErrorContext of
	 * the main process
	 *
	 * This is a thread-specific context and is not shared between other
	 * threads
	 */
	thrinfo->thr_error_context = AllocSetContextCreate(ErrorContext,
													   "ErrorContext",
													   8 * 1024,
													   8 * 1024,
													   8 * 1024,
													   false);

	thrinfo->thr_startroutine = startroutine;

	/*
	 * Now start the thread. The thread will start executing the given
	 * "startroutine". The thrinfo structure is also passed to the thread. Any
	 * additional parameters should be passed via the thrinfo strcuture.
	 *
	 * Return the thrinfo structure to the caller
	 */
	if ((err = pthread_create(&thrinfo->thr_id, NULL, GTM_ThreadMainWrapper,
							  thrinfo)))
	{
		ereport(LOG,
				(err,
				 errmsg("Failed to create a new thread: error %s", strerror(err))));

		MemoryContextDelete(thrinfo->thr_error_context);
		MemoryContextDelete(thrinfo->thr_thread_context);

		GTM_RWLockDestroy(&thrinfo->thr_lock);

		GTM_ThreadRemove(thrinfo);

		return NULL;
	}

	/*
	 * Ensure that the resources are released when the thread exits. (We used
	 * to do this inside GTM_ThreadMainWrapper, but thrinfo->thr_id may not set
	 * by the time GTM_ThreadMainWrapper starts executing, this possibly
	 * calling the function on an invalid thr_id
	 */
	pthread_detach(thrinfo->thr_id);

	return thrinfo;
}

/*
 * Exit the current thread
 */
void
GTM_ThreadExit(void)
{
	/* XXX To be implemented */
}

int
GTM_ThreadJoin(GTM_ThreadInfo *thrinfo)
{
	int error;
	void *data;

	error = pthread_join(thrinfo->thr_id, &data);

	return error;
}

/*
 * Get thread information for the given thread, identified by the
 * thread_id
 */
GTM_ThreadInfo *
GTM_GetThreadInfo(GTM_ThreadID thrid)
{

	return NULL;
}

/*
 * Cleanup routine for the thread
 */
static void
GTM_ThreadCleanup(void *argp)
{
	GTM_ThreadInfo *thrinfo = (GTM_ThreadInfo *)argp;

	elog(DEBUG1, "Cleaning up thread state");

	if (thrinfo->thr_status == GTM_THREAD_BACKUP)
	{
		int 			ii;

		for (ii = 0; ii < GTMThreads->gt_array_size; ii++)
		{
			if (GTMThreads->gt_threads[ii] && GTMThreads->gt_threads[ii] != thrinfo)
				GTM_RWLockRelease(&GTMThreads->gt_threads[ii]->thr_lock);
		}
	}

	/*
	 * Close a connection to GTM standby.
	 */
	if (thrinfo->thr_conn->standby)
	{
		elog(DEBUG1, "Closing a connection to the GTM standby.");

		GTMPQfinish(thrinfo->thr_conn->standby);
		thrinfo->thr_conn->standby = NULL;
	}

	/*
	 * TODO Close the open connection.
	 */
	StreamClose(thrinfo->thr_conn->con_port->sock);

	/* Free the node_name in the port */
	if (thrinfo->thr_conn->con_port->node_name != NULL)
		/* 
		 * We don't have to reset pointer to NULL her because ConnFree() 
		 * frees this structure next.
		 */
		pfree(thrinfo->thr_conn->con_port->node_name);

	/* Free the port */
	ConnFree(thrinfo->thr_conn->con_port);
	thrinfo->thr_conn->con_port = NULL;

	/* Free the connection info structure */
	pfree(thrinfo->thr_conn);
	thrinfo->thr_conn = NULL;

	/*
	 * Switch to the memory context of the main process so that we can free up
	 * our memory contextes easily.
	 *
	 * XXX We don't setup cleanup handlers for the main process. So this
	 * routine would never be called for the main process/thread.
	 */
	MemoryContextSwitchTo(thrinfo->thr_parent_context);

	MemoryContextDelete(thrinfo->thr_message_context);
	thrinfo->thr_message_context = NULL;

	MemoryContextDelete(thrinfo->thr_error_context);
	thrinfo->thr_error_context = NULL;

	MemoryContextDelete(thrinfo->thr_thread_context);
	thrinfo->thr_thread_context = NULL;

	GTM_RWLockDestroy(&thrinfo->thr_lock);

	/*
	 * TODO Now cleanup the thrinfo structure itself and remove it from the global
	 * array.
	 */
	GTM_ThreadRemove(thrinfo);

	/*
	 * Reset the thread-specific information. This should be done only after we
	 * are sure that memory contextes are not required.
	 *
	 * Note: elog calls need memory contextes, so no elog calls beyond this
	 * point.
	 */
	SetMyThreadInfo(NULL);

	return;
}

/*
 * A wrapper around the start routine of the thread. This helps us doing any
 * initialization and setting up cleanup handlers before the main routine is
 * started.
 */
void *
GTM_ThreadMainWrapper(void *argp)
{
	GTM_ThreadInfo *thrinfo = (GTM_ThreadInfo *)argp;

	SetMyThreadInfo(thrinfo);
	MemoryContextSwitchTo(TopMemoryContext);

	pthread_cleanup_push(GTM_ThreadCleanup, thrinfo);
	thrinfo->thr_startroutine(thrinfo);
	pthread_cleanup_pop(1);

	return thrinfo;
}

void
GTM_LockAllOtherThreads(void)
{
	GTM_ThreadInfo *my_threadinfo = GetMyThreadInfo;
	int ii;

	for (ii = 0; ii < GTMThreads->gt_array_size; ii++)
	{
		if (GTMThreads->gt_threads[ii] && GTMThreads->gt_threads[ii] != my_threadinfo)
			GTM_RWLockAcquire(&GTMThreads->gt_threads[ii]->thr_lock, GTM_LOCKMODE_WRITE);
	}
}

void
GTM_UnlockAllOtherThreads(void)
{
	GTM_ThreadInfo *my_threadinfo = GetMyThreadInfo;
	int ii;

	for (ii = 0; ii < GTMThreads->gt_array_size; ii++)
	{
		if (GTMThreads->gt_threads[ii] && GTMThreads->gt_threads[ii] != my_threadinfo)
			GTM_RWLockRelease(&GTMThreads->gt_threads[ii]->thr_lock);
	}
}

void
GTM_DoForAllOtherThreads(void (* process_routine)(GTM_ThreadInfo *))
{
	GTM_ThreadInfo *my_threadinfo = GetMyThreadInfo;
	int ii;

	for (ii = 0; ii < GTMThreads->gt_array_size; ii++)
	{
		if (GTMThreads->gt_threads[ii] && GTMThreads->gt_threads[ii] != my_threadinfo)
			(process_routine)(GTMThreads->gt_threads[ii]);
	}
}

/*
 * Get the latest client identifier from the list of open transactions and set
 * the next client identifier to be issued by us appropriately. Also remember
 * the latest client identifier separately since it will be used to check any
 * stale identifiers once we take over and old clients reconnect
 */
void
GTM_SetInitialAndNextClientIdentifierAtPromote(void)
{
	GTM_RWLockAcquire(&GTMThreads->gt_lock, GTM_LOCKMODE_WRITE);
	GTMThreads->gt_starting_client_id = GTMGetLastClientIdentifier();
	GTMThreads->gt_next_client_id =
		GTM_CLIENT_ID_NEXT(GTMThreads->gt_starting_client_id);
	GTM_RWLockRelease(&GTMThreads->gt_lock);
}
