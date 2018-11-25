/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#if defined (MSSD_DSM)
#include "mssd.h"
extern FILE* my_fp8;
extern MSSD_MAP* mssd_map;
extern pthread_t mssd_tid;
extern pthread_mutex_t mssd_mutex1;
extern pthread_cond_t mssd_cond1;
extern bool my_is_mssd_running;
#endif //MSSD_DSM
static int __ckpt_server_start(WT_CONNECTION_IMPL *);

/*
 * __ckpt_server_config --
 *	Parse and setup the checkpoint server options.
 */
static int
__ckpt_server_config(WT_SESSION_IMPL *session, const char **cfg, bool *startp)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	char *p;

	conn = S2C(session);

	/*
	 * The checkpoint configuration requires a wait time and/or a log
	 * size -- if one is not set, we're not running at all.
	 * Checkpoints based on log size also require logging be enabled.
	 */
	WT_RET(__wt_config_gets(session, cfg, "checkpoint.wait", &cval));
	conn->ckpt_usecs = (uint64_t)cval.val * WT_MILLION;

	WT_RET(__wt_config_gets(session, cfg, "checkpoint.log_size", &cval));
	conn->ckpt_logsize = (wt_off_t)cval.val;

	/* Checkpoints are incompatible with in-memory configuration */
	if (conn->ckpt_usecs != 0 || conn->ckpt_logsize != 0) {
		WT_RET(__wt_config_gets(session, cfg, "in_memory", &cval));
		if (cval.val != 0)
			WT_RET_MSG(session, EINVAL,
			    "In memory configuration incompatible with "
			    "checkpoints");
	}

	__wt_log_written_reset(session);
	if ((conn->ckpt_usecs == 0 && conn->ckpt_logsize == 0) ||
	    (conn->ckpt_logsize && conn->ckpt_usecs == 0 &&
	     !FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))) {
		*startp = false;
		return (0);
	}
	*startp = true;

	/*
	 * The application can specify a checkpoint name, which we ignore if
	 * it's our default.
	 */
	WT_RET(__wt_config_gets(session, cfg, "checkpoint.name", &cval));
	if (cval.len != 0 &&
	    !WT_STRING_MATCH(WT_CHECKPOINT, cval.str, cval.len)) {
		WT_RET(__wt_checkpoint_name_ok(session, cval.str, cval.len));

		WT_RET(__wt_scr_alloc(session, cval.len + 20, &tmp));
		WT_ERR(__wt_buf_fmt(
		    session, tmp, "name=%.*s", (int)cval.len, cval.str));
		WT_ERR(__wt_strdup(session, tmp->data, &p));

		__wt_free(session, conn->ckpt_config);
		conn->ckpt_config = p;
	}

err:	__wt_scr_free(session, &tmp);
	return (ret);
}

#if defined (MSSD_DSM)
static WT_THREAD_RET 
__mssd_map_thread(void* arg) {

	while (my_is_mssd_running) {
		//wait for pthread_cond_signal
		pthread_cond_wait(&mssd_cond1, &mssd_mutex1);
		// wait for other thread weak me up ...
		
		//wakeup by another thread
		mssdmap_flexmap(mssd_map, my_fp8);		

	} //end while
	printf("=============> inside MSSD handle thread, end WHILE \n");
	pthread_exit(NULL);
	return (WT_THREAD_RET_VALUE);
}	
#endif //MSSD_DSM
/*
 * __ckpt_server --
 *	The checkpoint server thread.
 */
static WT_THREAD_RET
__ckpt_server(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *session;

#if defined (MSSD_DSM)
	int ret_tem;
#endif

	session = arg;
	conn = S2C(session);
	wt_session = (WT_SESSION *)session;

	while (F_ISSET(conn, WT_CONN_SERVER_RUN) &&
	    F_ISSET(conn, WT_CONN_SERVER_CHECKPOINT)) {
		/*
		 * Wait...
		 * NOTE: If the user only configured logsize, then usecs
		 * will be 0 and this wait won't return until signalled.
		 */
		WT_ERR(
		    __wt_cond_wait(session, conn->ckpt_cond, conn->ckpt_usecs));
#if defined (MSSD_DSM)
		//Before call checkpoint, signal the __mssd_map_thread() to do the stream mapping 
		ret_tem = pthread_mutex_trylock(&mssd_mutex1);
		if (ret_tem == 0) {
			pthread_cond_signal(&mssd_cond1);
			pthread_mutex_unlock(&mssd_mutex1);
		}
		else {
		}
#endif //MSSD_DSM

		/* Checkpoint the database. */
		WT_ERR(wt_session->checkpoint(wt_session, conn->ckpt_config));

		/* Reset. */
		if (conn->ckpt_logsize) {
			__wt_log_written_reset(session);
			conn->ckpt_signalled = 0;

			/*
			 * In case we crossed the log limit during the
			 * checkpoint and the condition variable was already
			 * signalled, do a tiny wait to clear it so we don't do
			 * another checkpoint immediately.
			 */
			WT_ERR(__wt_cond_wait(session, conn->ckpt_cond, 1));
		}
	}

	if (0) {
err:		WT_PANIC_MSG(session, ret, "checkpoint server error");
	}
	return (WT_THREAD_RET_VALUE);
}

/*
 * __ckpt_server_start --
 *	Start the checkpoint server thread.
 */
static int
__ckpt_server_start(WT_CONNECTION_IMPL *conn)
{
	WT_SESSION_IMPL *session;
	uint32_t session_flags;

	/* Nothing to do if the server is already running. */
	if (conn->ckpt_session != NULL)
		return (0);

	F_SET(conn, WT_CONN_SERVER_CHECKPOINT);

	/*
	 * The checkpoint server gets its own session.
	 *
	 * Checkpoint does enough I/O it may be called upon to perform slow
	 * operations for the block manager.
	 */
	session_flags = WT_SESSION_CAN_WAIT;
	WT_RET(__wt_open_internal_session(conn,
	    "checkpoint-server", true, session_flags, &conn->ckpt_session));
	session = conn->ckpt_session;

	WT_RET(__wt_cond_alloc(
	    session, "checkpoint server", false, &conn->ckpt_cond));

	/*
	 * Start the thread.
	 */
	WT_RET(__wt_thread_create(
	    session, &conn->ckpt_tid, __ckpt_server, session));
	conn->ckpt_tid_set = true;

	return (0);
}

/*
 * __wt_checkpoint_server_create --
 *	Configure and start the checkpoint server.
 */
int
__wt_checkpoint_server_create(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	bool start;

	conn = S2C(session);
	start = false;

	/* If there is already a server running, shut it down. */
	if (conn->ckpt_session != NULL)
		WT_RET(__wt_checkpoint_server_destroy(session));

	WT_RET(__ckpt_server_config(session, cfg, &start));
	if (start)
		WT_RET(__ckpt_server_start(conn));

	return (0);
}

/*
 * __wt_checkpoint_server_destroy --
 *	Destroy the checkpoint server thread.
 */
int
__wt_checkpoint_server_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	conn = S2C(session);

	F_CLR(conn, WT_CONN_SERVER_CHECKPOINT);
	if (conn->ckpt_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->ckpt_cond));
		WT_TRET(__wt_thread_join(session, conn->ckpt_tid));
		conn->ckpt_tid_set = false;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->ckpt_cond));

	__wt_free(session, conn->ckpt_config);

	/* Close the server thread's session. */
	if (conn->ckpt_session != NULL) {
		wt_session = &conn->ckpt_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
	}

	/*
	 * Ensure checkpoint settings are cleared - so that reconfigure doesn't
	 * get confused.
	 */
	conn->ckpt_session = NULL;
	conn->ckpt_tid_set = false;
	conn->ckpt_cond = NULL;
	conn->ckpt_config = NULL;
	conn->ckpt_usecs = 0;

	return (ret);
}

/*
 * __wt_checkpoint_signal --
 *	Signal the checkpoint thread if sufficient log has been written.
 *	Return 1 if this signals the checkpoint thread, 0 otherwise.
 */
int
__wt_checkpoint_signal(WT_SESSION_IMPL *session, wt_off_t logsize)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	WT_ASSERT(session, WT_CKPT_LOGSIZE(conn));
	if (logsize >= conn->ckpt_logsize && !conn->ckpt_signalled) {
		WT_RET(__wt_cond_signal(session, conn->ckpt_cond));
		conn->ckpt_signalled = 1;
	}
	return (0);
}
