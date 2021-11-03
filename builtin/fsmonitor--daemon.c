#include "builtin.h"
#include "config.h"
#include "parse-options.h"
#include "fsmonitor.h"
#include "fsmonitor-ipc.h"
#include "compat/fsmonitor/fsm-listen.h"
#include "fsmonitor--daemon.h"
#include "simple-ipc.h"
#include "khash.h"

static const char * const builtin_fsmonitor__daemon_usage[] = {
	N_("git fsmonitor--daemon start [<options>]"),
	N_("git fsmonitor--daemon run [<options>]"),
	N_("git fsmonitor--daemon stop"),
	N_("git fsmonitor--daemon status"),
	NULL
};

#ifdef HAVE_FSMONITOR_DAEMON_BACKEND
/*
 * Global state loaded from config.
 */
#define FSMONITOR__IPC_THREADS "fsmonitor.ipcthreads"
static int fsmonitor__ipc_threads = 8;

#define FSMONITOR__START_TIMEOUT "fsmonitor.starttimeout"
static int fsmonitor__start_timeout_sec = 60;

static int fsmonitor_config(const char *var, const char *value, void *cb)
{
	if (!strcmp(var, FSMONITOR__IPC_THREADS)) {
		int i = git_config_int(var, value);
		if (i < 1)
			return error(_("value of '%s' out of range: %d"),
				     FSMONITOR__IPC_THREADS, i);
		fsmonitor__ipc_threads = i;
		return 0;
	}

	if (!strcmp(var, FSMONITOR__START_TIMEOUT)) {
		int i = git_config_int(var, value);
		if (i < 0)
			return error(_("value of '%s' out of range: %d"),
				     FSMONITOR__START_TIMEOUT, i);
		fsmonitor__start_timeout_sec = i;
		return 0;
	}

	return git_default_config(var, value, cb);
}

/*
 * Acting as a CLIENT.
 *
 * Send a "quit" command to the `git-fsmonitor--daemon` (if running)
 * and wait for it to shutdown.
 */
static int do_as_client__send_stop(void)
{
	struct strbuf answer = STRBUF_INIT;
	int ret;

	ret = fsmonitor_ipc__send_command("quit", &answer);

	/* The quit command does not return any response data. */
	strbuf_release(&answer);

	if (ret)
		return ret;

	trace2_region_enter("fsm_client", "polling-for-daemon-exit", NULL);
	while (fsmonitor_ipc__get_state() == IPC_STATE__LISTENING)
		sleep_millisec(50);
	trace2_region_leave("fsm_client", "polling-for-daemon-exit", NULL);

	return 0;
}

static int do_as_client__status(void)
{
	enum ipc_active_state state = fsmonitor_ipc__get_state();

	switch (state) {
	case IPC_STATE__LISTENING:
		printf(_("fsmonitor-daemon is watching '%s'\n"),
		       the_repository->worktree);
		return 0;

	default:
		printf(_("fsmonitor-daemon is not watching '%s'\n"),
		       the_repository->worktree);
		return 1;
	}
}

/*
 * Requests to and from a FSMonitor Protocol V2 provider use an opaque
 * "token" as a virtual timestamp.  Clients can request a summary of all
 * created/deleted/modified files relative to a token.  In the response,
 * clients receive a new token for the next (relative) request.
 *
 *
 * Token Format
 * ============
 *
 * The contents of the token are private and provider-specific.
 *
 * For the built-in fsmonitor--daemon, we define a token as follows:
 *
 *     "builtin" ":" <token_id> ":" <sequence_nr>
 *
 * The "builtin" prefix is used as a namespace to avoid conflicts
 * with other providers (such as Watchman).
 *
 * The <token_id> is an arbitrary OPAQUE string, such as a GUID,
 * UUID, or {timestamp,pid}.  It is used to group all filesystem
 * events that happened while the daemon was monitoring (and in-sync
 * with the filesystem).
 *
 *     Unlike FSMonitor Protocol V1, it is not defined as a timestamp
 *     and does not define less-than/greater-than relationships.
 *     (There are too many race conditions to rely on file system
 *     event timestamps.)
 *
 * The <sequence_nr> is a simple integer incremented whenever the
 * daemon needs to make its state public.  For example, if 1000 file
 * system events come in, but no clients have requested the data,
 * the daemon can continue to accumulate file changes in the same
 * bin and does not need to advance the sequence number.  However,
 * as soon as a client does arrive, the daemon needs to start a new
 * bin and increment the sequence number.
 *
 *     The sequence number serves as the boundary between 2 sets
 *     of bins -- the older ones that the client has already seen
 *     and the newer ones that it hasn't.
 *
 * When a new <token_id> is created, the <sequence_nr> is reset to
 * zero.
 *
 *
 * About Token Ids
 * ===============
 *
 * A new token_id is created:
 *
 * [1] each time the daemon is started.
 *
 * [2] any time that the daemon must re-sync with the filesystem
 *     (such as when the kernel drops or we miss events on a very
 *     active volume).
 *
 * [3] in response to a client "flush" command (for dropped event
 *     testing).
 *
 * When a new token_id is created, the daemon is free to discard all
 * cached filesystem events associated with any previous token_ids.
 * Events associated with a non-current token_id will never be sent
 * to a client.  A token_id change implicitly means that the daemon
 * has gap in its event history.
 *
 * Therefore, clients that present a token with a stale (non-current)
 * token_id will always be given a trivial response.
 */
struct fsmonitor_token_data {
	struct strbuf token_id;
	struct fsmonitor_batch *batch_head;
	struct fsmonitor_batch *batch_tail;
	uint64_t client_ref_count;
};

struct fsmonitor_batch {
	struct fsmonitor_batch *next;
	uint64_t batch_seq_nr;
	const char **interned_paths;
	size_t nr, alloc;
	time_t pinned_time;
};

static struct fsmonitor_token_data *fsmonitor_new_token_data(void)
{
	static int test_env_value = -1;
	static uint64_t flush_count = 0;
	struct fsmonitor_token_data *token;
	struct fsmonitor_batch *batch;

	CALLOC_ARRAY(token, 1);
	batch = fsmonitor_batch__new();

	strbuf_init(&token->token_id, 0);
	token->batch_head = batch;
	token->batch_tail = batch;
	token->client_ref_count = 0;

	if (test_env_value < 0)
		test_env_value = git_env_bool("GIT_TEST_FSMONITOR_TOKEN", 0);

	if (!test_env_value) {
		struct timeval tv;
		struct tm tm;
		time_t secs;

		gettimeofday(&tv, NULL);
		secs = tv.tv_sec;
		gmtime_r(&secs, &tm);

		strbuf_addf(&token->token_id,
			    "%"PRIu64".%d.%4d%02d%02dT%02d%02d%02d.%06ldZ",
			    flush_count++,
			    getpid(),
			    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			    tm.tm_hour, tm.tm_min, tm.tm_sec,
			    (long)tv.tv_usec);
	} else {
		strbuf_addf(&token->token_id, "test_%08x", test_env_value++);
	}

	/*
	 * We created a new <token_id> and are starting a new series
	 * of tokens with a zero <seq_nr>.
	 *
	 * Since clients cannot guess our new (non test) <token_id>
	 * they will always receive a trivial response (because of the
	 * mismatch on the <token_id>).  The trivial response will
	 * tell them our new <token_id> so that subsequent requests
	 * will be relative to our new series.  (And when sending that
	 * response, we pin the current head of the batch list.)
	 *
	 * Even if the client correctly guesses the <token_id>, their
	 * request of "builtin:<token_id>:0" asks for all changes MORE
	 * RECENT than batch/bin 0.
	 *
	 * This implies that it is a waste to accumulate paths in the
	 * initial batch/bin (because they will never be transmitted).
	 *
	 * So the daemon could be running for days and watching the
	 * file system, but doesn't need to actually accumulate any
	 * paths UNTIL we need to set a reference point for a later
	 * relative request.
	 *
	 * However, it is very useful for testing to always have a
	 * reference point set.  Pin batch 0 to force early file system
	 * events to accumulate.
	 */
	if (test_env_value)
		batch->pinned_time = time(NULL);

	return token;
}

struct fsmonitor_batch *fsmonitor_batch__new(void)
{
	struct fsmonitor_batch *batch;

	CALLOC_ARRAY(batch, 1);

	return batch;
}

void fsmonitor_batch__free_list(struct fsmonitor_batch *batch)
{
	while (batch) {
		struct fsmonitor_batch *next = batch->next;

		/*
		 * The actual strings within the array of this batch
		 * are interned, so we don't own them.  We only own
		 * the array.
		 */
		free(batch->interned_paths);
		free(batch);

		batch = next;
	}
}

void fsmonitor_batch__add_path(struct fsmonitor_batch *batch,
			       const char *path)
{
	const char *interned_path = strintern(path);

	trace_printf_key(&trace_fsmonitor, "event: %s", interned_path);

	ALLOC_GROW(batch->interned_paths, batch->nr + 1, batch->alloc);
	batch->interned_paths[batch->nr++] = interned_path;
}

static void fsmonitor_batch__combine(struct fsmonitor_batch *batch_dest,
				     const struct fsmonitor_batch *batch_src)
{
	size_t k;

	ALLOC_GROW(batch_dest->interned_paths,
		   batch_dest->nr + batch_src->nr + 1,
		   batch_dest->alloc);

	for (k = 0; k < batch_src->nr; k++)
		batch_dest->interned_paths[batch_dest->nr++] =
			batch_src->interned_paths[k];
}

static void fsmonitor_free_token_data(struct fsmonitor_token_data *token)
{
	if (!token)
		return;

	assert(token->client_ref_count == 0);

	strbuf_release(&token->token_id);

	fsmonitor_batch__free_list(token->batch_head);

	free(token);
}

/*
 * Flush all of our cached data about the filesystem.  Call this if we
 * lose sync with the filesystem and miss some notification events.
 *
 * [1] If we are missing events, then we no longer have a complete
 *     history of the directory (relative to our current start token).
 *     We should create a new token and start fresh (as if we just
 *     booted up).
 *
 * If there are no concurrent threads readering the current token data
 * series, we can free it now.  Otherwise, let the last reader free
 * it.
 *
 * Either way, the old token data series is no longer associated with
 * our state data.
 */
static void with_lock__do_force_resync(struct fsmonitor_daemon_state *state)
{
	/* assert current thread holding state->main_lock */

	struct fsmonitor_token_data *free_me = NULL;
	struct fsmonitor_token_data *new_one = NULL;

	new_one = fsmonitor_new_token_data();

	if (state->current_token_data->client_ref_count == 0)
		free_me = state->current_token_data;
	state->current_token_data = new_one;

	fsmonitor_free_token_data(free_me);
}

void fsmonitor_force_resync(struct fsmonitor_daemon_state *state)
{
	pthread_mutex_lock(&state->main_lock);
	with_lock__do_force_resync(state);
	pthread_mutex_unlock(&state->main_lock);
}

static ipc_server_application_cb handle_client;

static int handle_client(void *data,
			 const char *command, size_t command_len,
			 ipc_server_reply_cb *reply,
			 struct ipc_server_reply_data *reply_data)
{
	/* struct fsmonitor_daemon_state *state = data; */
	int result;

	/*
	 * The Simple IPC API now supports {char*, len} arguments, but
	 * FSMonitor always uses proper null-terminated strings, so
	 * we can ignore the command_len argument.  (Trust, but verify.)
	 */
	if (command_len != strlen(command))
		BUG("FSMonitor assumes text messages");

	trace2_region_enter("fsmonitor", "handle_client", the_repository);
	trace2_data_string("fsmonitor", the_repository, "request", command);

	result = 0; /* TODO Do something here. */

	trace2_region_leave("fsmonitor", "handle_client", the_repository);

	return result;
}

#define FSMONITOR_COOKIE_PREFIX ".fsmonitor-daemon-"

enum fsmonitor_path_type fsmonitor_classify_path_workdir_relative(
	const char *rel)
{
	if (fspathncmp(rel, ".git", 4))
		return IS_WORKDIR_PATH;
	rel += 4;

	if (!*rel)
		return IS_DOT_GIT;
	if (*rel != '/')
		return IS_WORKDIR_PATH; /* e.g. .gitignore */
	rel++;

	if (!fspathncmp(rel, FSMONITOR_COOKIE_PREFIX,
			strlen(FSMONITOR_COOKIE_PREFIX)))
		return IS_INSIDE_DOT_GIT_WITH_COOKIE_PREFIX;

	return IS_INSIDE_DOT_GIT;
}

enum fsmonitor_path_type fsmonitor_classify_path_gitdir_relative(
	const char *rel)
{
	if (!fspathncmp(rel, FSMONITOR_COOKIE_PREFIX,
			strlen(FSMONITOR_COOKIE_PREFIX)))
		return IS_INSIDE_GITDIR_WITH_COOKIE_PREFIX;

	return IS_INSIDE_GITDIR;
}

static enum fsmonitor_path_type try_classify_workdir_abs_path(
	struct fsmonitor_daemon_state *state,
	const char *path)
{
	const char *rel;

	if (fspathncmp(path, state->path_worktree_watch.buf,
		       state->path_worktree_watch.len))
		return IS_OUTSIDE_CONE;

	rel = path + state->path_worktree_watch.len;

	if (!*rel)
		return IS_WORKDIR_PATH; /* it is the root dir exactly */
	if (*rel != '/')
		return IS_OUTSIDE_CONE;
	rel++;

	return fsmonitor_classify_path_workdir_relative(rel);
}

enum fsmonitor_path_type fsmonitor_classify_path_absolute(
	struct fsmonitor_daemon_state *state,
	const char *path)
{
	const char *rel;
	enum fsmonitor_path_type t;

	t = try_classify_workdir_abs_path(state, path);
	if (state->nr_paths_watching == 1)
		return t;
	if (t != IS_OUTSIDE_CONE)
		return t;

	if (fspathncmp(path, state->path_gitdir_watch.buf,
		       state->path_gitdir_watch.len))
		return IS_OUTSIDE_CONE;

	rel = path + state->path_gitdir_watch.len;

	if (!*rel)
		return IS_GITDIR; /* it is the <gitdir> exactly */
	if (*rel != '/')
		return IS_OUTSIDE_CONE;
	rel++;

	return fsmonitor_classify_path_gitdir_relative(rel);
}

/*
 * We try to combine small batches at the front of the batch-list to avoid
 * having a long list.  This hopefully makes it a little easier when we want
 * to truncate and maintain the list.  However, we don't want the paths array
 * to just keep growing and growing with realloc, so we insert an arbitrary
 * limit.
 */
#define MY_COMBINE_LIMIT (1024)

void fsmonitor_publish(struct fsmonitor_daemon_state *state,
		       struct fsmonitor_batch *batch,
		       const struct string_list *cookie_names)
{
	if (!batch && !cookie_names->nr)
		return;

	pthread_mutex_lock(&state->main_lock);

	if (batch) {
		struct fsmonitor_batch *head;

		head = state->current_token_data->batch_head;
		if (!head) {
			BUG("token does not have batch");
		} else if (head->pinned_time) {
			/*
			 * We cannot alter the current batch list
			 * because:
			 *
			 * [a] it is being transmitted to at least one
			 * client and the handle_client() thread has a
			 * ref-count, but not a lock on the batch list
			 * starting with this item.
			 *
			 * [b] it has been transmitted in the past to
			 * at least one client such that future
			 * requests are relative to this head batch.
			 *
			 * So, we can only prepend a new batch onto
			 * the front of the list.
			 */
			batch->batch_seq_nr = head->batch_seq_nr + 1;
			batch->next = head;
			state->current_token_data->batch_head = batch;
		} else if (!head->batch_seq_nr) {
			/*
			 * Batch 0 is unpinned.  See the note in
			 * `fsmonitor_new_token_data()` about why we
			 * don't need to accumulate these paths.
			 */
			fsmonitor_batch__free_list(batch);
		} else if (head->nr + batch->nr > MY_COMBINE_LIMIT) {
			/*
			 * The head batch in the list has never been
			 * transmitted to a client, but folding the
			 * contents of the new batch onto it would
			 * exceed our arbitrary limit, so just prepend
			 * the new batch onto the list.
			 */
			batch->batch_seq_nr = head->batch_seq_nr + 1;
			batch->next = head;
			state->current_token_data->batch_head = batch;
		} else {
			/*
			 * We are free to add the paths in the given
			 * batch onto the end of the current head batch.
			 */
			fsmonitor_batch__combine(head, batch);
			fsmonitor_batch__free_list(batch);
		}
	}

	pthread_mutex_unlock(&state->main_lock);
}

static void *fsm_listen__thread_proc(void *_state)
{
	struct fsmonitor_daemon_state *state = _state;

	trace2_thread_start("fsm-listen");

	trace_printf_key(&trace_fsmonitor, "Watching: worktree '%s'",
			 state->path_worktree_watch.buf);
	if (state->nr_paths_watching > 1)
		trace_printf_key(&trace_fsmonitor, "Watching: gitdir '%s'",
				 state->path_gitdir_watch.buf);

	fsm_listen__loop(state);

	pthread_mutex_lock(&state->main_lock);
	if (state->current_token_data &&
	    state->current_token_data->client_ref_count == 0)
		fsmonitor_free_token_data(state->current_token_data);
	state->current_token_data = NULL;
	pthread_mutex_unlock(&state->main_lock);

	trace2_thread_exit();
	return NULL;
}

static int fsmonitor_run_daemon_1(struct fsmonitor_daemon_state *state)
{
	struct ipc_server_opts ipc_opts = {
		.nr_threads = fsmonitor__ipc_threads,

		/*
		 * We know that there are no other active threads yet,
		 * so we can let the IPC layer temporarily chdir() if
		 * it needs to when creating the server side of the
		 * Unix domain socket.
		 */
		.uds_disallow_chdir = 0
	};

	/*
	 * Start the IPC thread pool before the we've started the file
	 * system event listener thread so that we have the IPC handle
	 * before we need it.
	 */
	if (ipc_server_run_async(&state->ipc_server_data,
				 fsmonitor_ipc__get_path(), &ipc_opts,
				 handle_client, state))
		return error_errno(
			_("could not start IPC thread pool on '%s'"),
			fsmonitor_ipc__get_path());

	/*
	 * Start the fsmonitor listener thread to collect filesystem
	 * events.
	 */
	if (pthread_create(&state->listener_thread, NULL,
			   fsm_listen__thread_proc, state) < 0) {
		ipc_server_stop_async(state->ipc_server_data);
		ipc_server_await(state->ipc_server_data);

		return error(_("could not start fsmonitor listener thread"));
	}

	/*
	 * The daemon is now fully functional in background threads.
	 * Wait for the IPC thread pool to shutdown (whether by client
	 * request or from filesystem activity).
	 */
	ipc_server_await(state->ipc_server_data);

	/*
	 * The fsmonitor listener thread may have received a shutdown
	 * event from the IPC thread pool, but it doesn't hurt to tell
	 * it again.  And wait for it to shutdown.
	 */
	fsm_listen__stop_async(state);
	pthread_join(state->listener_thread, NULL);

	return state->error_code;
}

static int fsmonitor_run_daemon(void)
{
	struct fsmonitor_daemon_state state;
	int err;

	memset(&state, 0, sizeof(state));

	pthread_mutex_init(&state.main_lock, NULL);
	state.error_code = 0;
	state.current_token_data = fsmonitor_new_token_data();

	/* Prepare to (recursively) watch the <worktree-root> directory. */
	strbuf_init(&state.path_worktree_watch, 0);
	strbuf_addstr(&state.path_worktree_watch, absolute_path(get_git_work_tree()));
	state.nr_paths_watching = 1;

	/*
	 * We create and delete cookie files somewhere inside the .git
	 * directory to help us keep sync with the file system.  If
	 * ".git" is not a directory, then <gitdir> is not inside the
	 * cone of <worktree-root>, so set up a second watch to watch
	 * the <gitdir> so that we get events for the cookie files.
	 */
	strbuf_init(&state.path_gitdir_watch, 0);
	strbuf_addbuf(&state.path_gitdir_watch, &state.path_worktree_watch);
	strbuf_addstr(&state.path_gitdir_watch, "/.git");
	if (!is_directory(state.path_gitdir_watch.buf)) {
		strbuf_reset(&state.path_gitdir_watch);
		strbuf_addstr(&state.path_gitdir_watch, absolute_path(get_git_dir()));
		state.nr_paths_watching = 2;
	}

	/*
	 * Confirm that we can create platform-specific resources for the
	 * filesystem listener before we bother starting all the threads.
	 */
	if (fsm_listen__ctor(&state)) {
		err = error(_("could not initialize listener thread"));
		goto done;
	}

	err = fsmonitor_run_daemon_1(&state);

done:
	pthread_mutex_destroy(&state.main_lock);
	fsm_listen__dtor(&state);

	ipc_server_free(state.ipc_server_data);

	strbuf_release(&state.path_worktree_watch);
	strbuf_release(&state.path_gitdir_watch);

	return err;
}

static int try_to_run_foreground_daemon(int free_console)
{
	/*
	 * Technically, we don't need to probe for an existing daemon
	 * process, since we could just call `fsmonitor_run_daemon()`
	 * and let it fail if the pipe/socket is busy.
	 *
	 * However, this method gives us a nicer error message for a
	 * common error case.
	 */
	if (fsmonitor_ipc__get_state() == IPC_STATE__LISTENING)
		die("fsmonitor--daemon is already running '%s'",
		    the_repository->worktree);

	printf(_("running fsmonitor-daemon in '%s'\n"),
	       the_repository->worktree);
	fflush(stdout);

#ifdef GIT_WINDOWS_NATIVE
	if (free_console)
		FreeConsole();
#endif

	return !!fsmonitor_run_daemon();
}

static start_bg_wait_cb bg_wait_cb;

static int bg_wait_cb(const struct child_process *cp, void *cb_data)
{
	enum ipc_active_state s = fsmonitor_ipc__get_state();

	switch (s) {
	case IPC_STATE__LISTENING:
		/* child is "ready" */
		return 0;

	case IPC_STATE__NOT_LISTENING:
	case IPC_STATE__PATH_NOT_FOUND:
		/* give child more time */
		return 1;

	default:
	case IPC_STATE__INVALID_PATH:
	case IPC_STATE__OTHER_ERROR:
		/* all the time in world won't help */
		return -1;
	}
}

static int try_to_start_background_daemon(void)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	enum start_bg_result sbgr;

	/*
	 * Before we try to create a background daemon process, see
	 * if a daemon process is already listening.  This makes it
	 * easier for us to report an already-listening error to the
	 * console, since our spawn/daemon can only report the success
	 * of creating the background process (and not whether it
	 * immediately exited).
	 */
	if (fsmonitor_ipc__get_state() == IPC_STATE__LISTENING)
		die("fsmonitor--daemon is already running '%s'",
		    the_repository->worktree);

	printf(_("starting fsmonitor-daemon in '%s'\n"),
	       the_repository->worktree);
	fflush(stdout);

	cp.git_cmd = 1;

	strvec_push(&cp.args, "fsmonitor--daemon");
	strvec_push(&cp.args, "run");
	strvec_push(&cp.args, "--free-console");
	strvec_pushf(&cp.args, "--ipc-threads=%d", fsmonitor__ipc_threads);

	cp.no_stdin = 1;
	cp.no_stdout = 1;
	cp.no_stderr = 1;

	sbgr = start_bg_command(&cp, bg_wait_cb, NULL,
				fsmonitor__start_timeout_sec);

	switch (sbgr) {
	case SBGR_READY:
		return 0;

	default:
	case SBGR_ERROR:
	case SBGR_CB_ERROR:
		return error("daemon failed to start");

	case SBGR_TIMEOUT:
		return error("daemon not online yet");

	case SBGR_DIED:
		return error("daemon terminated");
	}
}

int cmd_fsmonitor__daemon(int argc, const char **argv, const char *prefix)
{
	const char *subcmd;
	int free_console = 0;

	struct option options[] = {
		OPT_BOOL(0, "free-console", &free_console, N_("free console")),
		OPT_INTEGER(0, "ipc-threads",
			    &fsmonitor__ipc_threads,
			    N_("use <n> ipc worker threads")),
		OPT_INTEGER(0, "start-timeout",
			    &fsmonitor__start_timeout_sec,
			    N_("Max seconds to wait for background daemon startup")),

		OPT_END()
	};

	git_config(fsmonitor_config, NULL);

	argc = parse_options(argc, argv, prefix, options,
			     builtin_fsmonitor__daemon_usage, 0);
	if (argc != 1)
		usage_with_options(builtin_fsmonitor__daemon_usage, options);
	subcmd = argv[0];

	if (fsmonitor__ipc_threads < 1)
		die(_("invalid 'ipc-threads' value (%d)"),
		    fsmonitor__ipc_threads);

	if (!strcmp(subcmd, "start"))
		return !!try_to_start_background_daemon();

	if (!strcmp(subcmd, "run"))
		return !!try_to_run_foreground_daemon(free_console);

	if (!strcmp(subcmd, "stop"))
		return !!do_as_client__send_stop();

	if (!strcmp(subcmd, "status"))
		return !!do_as_client__status();

	die(_("Unhandled subcommand '%s'"), subcmd);
}

#else
int cmd_fsmonitor__daemon(int argc, const char **argv, const char *prefix)
{
	struct option options[] = {
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_fsmonitor__daemon_usage, options);

	die(_("fsmonitor--daemon not supported on this platform"));
}
#endif
