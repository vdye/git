#include "builtin.h"
#include "config.h"
#include "object-store.h"
#include "oidmap.h"
#include "parse-options.h"
#include "simple-ipc.h"
#include "strbuf.h"
#include "thread-utils.h"
#include "odb-over-ipc.h"
#include "trace2.h"

enum my_mode {
	MODE_UNDEFINED = 0,
	MODE_RUN, /* run daemon in current process */
	MODE_STOP, /* stop an existing daemon */
};

static const char * const my_usage[] = {
	"git odb--daemon --run  [<options>]",
	"git odb--daemon --stop [<options>]",
	"git odb--daemon TBD...",
	NULL
};

struct my_args
{
	enum my_mode mode;
	int nr_ipc_threads;
};

static struct my_args my_args;

static struct option my_options[] = {
	OPT_CMDMODE(0, "run", &my_args.mode,
		    N_("run the ODB daemon in the foreground"), MODE_RUN),
	OPT_CMDMODE(0, "stop", &my_args.mode,
		    N_("stop an existing ODB daemon"), MODE_STOP),

	OPT_GROUP(N_("Daemon options")),
	OPT_INTEGER(0, "ipc-threads", &my_args.nr_ipc_threads, N_("use <n> ipc threads")),
	OPT_END()
};

#ifndef SUPPORTS_SIMPLE_IPC
int cmd_odb__daemon(int argc, const char **argv, const char *prefix)
{
	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(my_usage, my_options);

	die(_("odb--daemon not supported on this platform"));
}
#else

/*
 * Technically, we don't need to probe for an existing daemon process
 * running on our named pipe / socket, since we could just try to
 * create it and let it fail if the pipe/socket is busy.  However,
 * probing first allows us to give a better error message (especially
 * in the case where we disassociate from the terminal and fork into
 * the background).
 */
static int is_daemon_listening(void)
{
	return odb_over_ipc__get_state() == IPC_STATE__LISTENING;
}

struct my_odb_ipc_state
{
	struct ipc_server_data *ipc_state;
};

static struct my_odb_ipc_state my_state;

struct my_oidmap_entry
{
	struct oidmap_entry entry;
	struct odb_over_ipc__get_oid__response resp;
	char *content;
};

static struct oidmap my_oidmap = OIDMAP_INIT;

static int do_oid_lookup(struct my_oidmap_entry *e,
			 struct odb_over_ipc__get_oid__request *req)
{
	struct object_info var_oi = OBJECT_INFO_INIT;
	int ret;

	var_oi.typep = &e->resp.type;
	var_oi.sizep = &e->resp.size;
	var_oi.disk_sizep = &e->resp.disk_size;
	var_oi.delta_base_oid = &e->resp.delta_base_oid;
	/* the client can compute `type_name` from `type`. */

	if (req->want_content)
		var_oi.contentp = (void **)&e->content;

	ret = oid_object_info_extended(the_repository, &req->oid, &var_oi,
				       req->flags);
	if (ret)
		return -1;

	// TODO should we create a new fake whence value that we report to
	// TODO the client -- something like "ipc" to indicate that the
	// TODO client got it from us and therefore doesn't have the in-memory
	// TODO cache nor any of the packfile data loaded.  The answer to this
	// TODO also affects the oi.u.packed fields.
	e->resp.whence = var_oi.whence;

	// TODO decide if we care about oi.u.packed

	return 0;
}

static int odb_ipc_cb__get_oid(struct my_odb_ipc_state *state,
			       const char *command, size_t command_len,
			       ipc_server_reply_cb *reply_cb,
			       struct ipc_server_reply_data *reply_data)
{
	struct odb_over_ipc__get_oid__request *req;
	struct my_oidmap_entry *e;

	if (command_len != sizeof(*req))
		BUG("incorrect size for binary data");

	req = (struct odb_over_ipc__get_oid__request *)command;

	e = oidmap_get(&my_oidmap, &req->oid);
//	{
//		char hexbuf[200];
//		trace2_printf("get-oid: %s %s", oid_to_hex_r(hexbuf, &req->oid),
//			      (e ? "found" : "not-found"));
//	}

	if (e && req->want_content && !e->content) {
		/*
		 * We have a cached entry from a previous request where
		 * the client did not want the content, but this client
		 * does want the content.  So repeat the lookup and ammend
		 * our cache entry.
		 */
		if (do_oid_lookup(e, req))
			goto fail;
	}

	if (!e) {
		e = xcalloc(1, sizeof(*e));

		memcpy(e->resp.key.key, "oid", 4);
		oidcpy(&e->resp.oid, &req->oid);
		oidclr(&e->resp.delta_base_oid);

		if (do_oid_lookup(e, req))
			goto fail;

		oidcpy(&e->entry.oid, &req->oid);
		oidmap_put(&my_oidmap, e);
	}

	reply_cb(reply_data, (const char *)&e->resp, sizeof(e->resp));
	if (req->want_content)
		reply_cb(reply_data, e->content, e->resp.size);

	return 0;

fail:
	/*
	 * Send the client an error response to force it to do
	 * the work itself.
	 */
	reply_cb(reply_data, "error", 6);
	return 0;
}

/*
 * This callback handles IPC requests from clients.  We run on an
 * arbitrary thread.
 *
 * We expect `command` to be of the form:
 *
 * <command> := quit NUL
 *            | TBC...
 */
static ipc_server_application_cb odb_ipc_cb;

static int odb_ipc_cb(void *data,
		      const char *command, size_t command_len,
		      ipc_server_reply_cb *reply_cb,
		      struct ipc_server_reply_data *reply_data)
{
	// TODO I did not take time to ensure that `command_len` is
	// TODO large enough to do all of the strcmp() and starts_with()
	// TODO calculations when I converted the IPC API to take
	// TODO `command, command_len` rather than just `command`.
	// TODO So some cleanup is needed here.

	struct my_odb_ipc_state *state = data;
	int ret;

	assert(state == &my_state);

	if (!strcmp(command, "quit")) {
		/*
		 * A client has requested (over the pipe/socket) that this
		 * daemon shutdown.
		 *
		 * Tell the IPC thread pool to shutdown (which completes the
		 * `await` in the main thread and causes us to shutdown).
		 *
		 * In this callback we are NOT in control of the life of this
		 * thread, so we cannot directly shutdown here.
		 */
		return SIMPLE_IPC_QUIT;
	}

	if (!strcmp(command, "oid")) {
		/*
		 * A client has requested that we lookup an object from the
		 * ODB and send it to them.
		 */
		trace2_region_enter("odb-daemon", "get-oid", NULL);
		ret = odb_ipc_cb__get_oid(state, command, command_len,
					  reply_cb, reply_data);
		trace2_region_leave("odb-daemon", "get-oid", NULL);

		return ret;
	}

	// TODO respond to other requests from client.
	//
	// TODO decide how to return an error for unknown commands.

	return 0;
}

static int launch_ipc_thread_pool(void)
{
	int ret;
	const char *path = odb_over_ipc__get_path();

	struct ipc_server_opts ipc_opts = {
		.nr_threads = my_args.nr_ipc_threads,
	};

	ret = ipc_server_run_async(&my_state.ipc_state,
				   path, &ipc_opts, odb_ipc_cb, &my_state);

	if (ret == -2) /* maybe we lost a startup race */
		error(_("IPC socket/pipe already in use: '%s'"), path);

	else if (ret == -1)
		error(_("could not start IPC thread pool on: '%s'"), path);

	return ret;
}

/*
 * Actually run the daemon.
 */
static int do_run_daemon(void)
{
	int ret;

	odb_over_ipc__set_is_daemon();

	oidmap_init(&my_oidmap, 1024 * 1024);
	hashmap_disable_item_counting(&my_oidmap.map);

	// TODO Create mutexes and locks
	//
	// TODO Load up the packfiles
	//
	// TODO Consider starting a thread to watch for new/deleted packfiles
	// TODO and update the in-memory database.

	ret = launch_ipc_thread_pool();
	if (!ret)
		ret = ipc_server_await(my_state.ipc_state);

	// TODO As a precaution, send stop events to our other threads and
	// TODO JOIN them.
	//
	// TODO destroy mutexes and locks.
	//
	// TODO destroy in-memory databases.

	return ret;
}

/*
 * Try to run the odb--daemon in the foreground of the current process.
 */
static int try_run_daemon(void)
{
	if (is_daemon_listening())
		die("odb--daemon is already running");

	return do_run_daemon();
}

/*
 * Acting as a CLIENT.
 *
 * Send a "quit" command to an existing `git odb--daemon` process
 * (if running) and wait for it to shutdown.
 *
 * The wait here is important for helping the test suite be stable.
 */
static int client_send_stop(void)
{
	struct strbuf answer = STRBUF_INIT;
	int ret;

	ret = odb_over_ipc__command("quit", 4, &answer);

	/* The quit command does not return any response data. */
	strbuf_release(&answer);

	if (ret) {
		die("could not send stop command to odb--daemon");
		return ret;
	}

	trace2_region_enter("odb_client", "polling-for-daemon-exit", NULL);
	while (odb_over_ipc__get_state() == IPC_STATE__LISTENING)
		sleep_millisec(50);
	trace2_region_leave("odb_client", "polling-for-daemon-exit", NULL);

	return 0;
}

int cmd_odb__daemon(int argc, const char **argv, const char *prefix)
{
	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(my_usage, my_options);

	git_config(git_default_config, NULL);

	argc = parse_options(argc, argv, prefix, my_options, my_usage, 0);

	if (my_args.nr_ipc_threads < 1)
		my_args.nr_ipc_threads = online_cpus();

	switch (my_args.mode) {
	case MODE_RUN:
		return !!try_run_daemon();

	case MODE_STOP:
		return !!client_send_stop();

	default:
		die(_("Unhandled command mode"));
	}
}
#endif /* SUPPORTS_SIMPLE_IPC */
