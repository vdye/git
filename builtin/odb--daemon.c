#include "builtin.h"
#include "config.h"
#include "object-store.h"
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

static int odb_ipc_cb__get_oid(struct my_odb_ipc_state *state,
			       const char *command,
			       ipc_server_reply_cb *reply_cb,
			       struct ipc_server_reply_data *reply_data)
{
	struct strbuf **lines = strbuf_split_str(command, '\n', 0);
	struct strbuf response = STRBUF_INIT;
	struct object_id oid;
	const char *sz;
	uintmax_t umax_flags = 0;
	int k;

	// trace2_printf("oid--daemon: received:\n%s", command);

	oidclr(&oid);

	for (k = 0; lines[k]; k++) {
		strbuf_trim_trailing_newline(lines[k]);

		if (skip_prefix(lines[k]->buf, "oid ", &sz)) {
			if (get_oid_hex(sz, &oid))
				goto fail;
			continue;
		}

		if (skip_prefix(lines[k]->buf, "flags ", &sz)) {
			umax_flags = strtoumax(sz, NULL, 10);
			continue;
		}

		BUG("unexpected line '%s' in OID request", lines[k]->buf);
	}

	// TODO Insert ODB lookup from in-memory database here.
	// TODO For now, just do the regular lookup.

	{
		enum object_type var_type = OBJ_BAD;
		unsigned long var_size = 0;
		off_t var_disk_size = 0;
		struct object_id var_delta_base_oid;
		void *var_content = NULL;
		struct object_info var_oi = OBJECT_INFO_INIT;
		int ret;

		oidclr(&var_delta_base_oid);

		var_oi.typep = &var_type;
		var_oi.sizep = &var_size;
		var_oi.disk_sizep = &var_disk_size;
		var_oi.delta_base_oid = &var_delta_base_oid;
		// TODO have the client send another field to indicate
		// TODO whether it wants the contents or not.
		var_oi.contentp = &var_content;

		ret = oid_object_info_extended(the_repository, &oid, &var_oi,
					       (unsigned)umax_flags);
		if (ret)
			goto fail;

		strbuf_reset(&response);
		strbuf_addf(&response, "oid %s\n", oid_to_hex(&oid));
		strbuf_addf(&response, "type %d\n", var_type);
		strbuf_addf(&response, "size %"PRIuMAX"\n", (uintmax_t)var_size);
		strbuf_addf(&response, "disk %"PRIuMAX"\n", (uintmax_t)var_disk_size);
		if (!is_null_oid(&var_delta_base_oid))
			strbuf_addf(&response, "delta %s\n", oid_to_hex(&var_delta_base_oid));
		// TODO should we create a new fake whence value that we report to
		// TODO the client -- something like "ipc" to indicate that the
		// TODO client got it from us and therefore doesn't have the in-memory
		// TODO cache nor any of the packfile data loaded.  The answer to this
		// TODO also affects the oi.u.packed fields.
		strbuf_addf(&response, "whence %d\n", var_oi.whence);

		// TODO decide if we care about oi.u.packed

		// trace2_printf("oid--daemon: sending:\n%s", response.buf);

		/*
		 * Add one to the length of the headers to include the NUL and
		 * then immediately send the object contents.
		 */
		reply_cb(reply_data, response.buf, response.len + 1);
		reply_cb(reply_data, var_content, var_size);

		free(var_content);
	}

	goto done;

fail:
	/*
	 * Send the client an error response to force it to do
	 * the work itself.
	 */
	strbuf_reset(&response);
	strbuf_addstr(&response, "error");
	reply_cb(reply_data, response.buf, response.len + 1);
	goto done;

done:
	if (lines)
		strbuf_list_free(lines);
	strbuf_release(&response);
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

static int odb_ipc_cb(void *data, const char *command,
		      ipc_server_reply_cb *reply_cb,
		      struct ipc_server_reply_data *reply_data)
{
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

	if (!strncmp(command, "oid", 3)) {
		/*
		 * A client has requested that we lookup an object from the
		 * ODB and send it to them.
		 */
		trace2_region_enter("odb-daemon", "get-oid", NULL);
		ret = odb_ipc_cb__get_oid(state, command, reply_cb, reply_data);
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

	ret = odb_over_ipc__command("quit", &answer);

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
