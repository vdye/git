#include "builtin.h"
#include "config.h"
#include "parse-options.h"
#include "simple-ipc.h"
#include "thread-utils.h"
#include "odb-over-ipc.h"

enum my_mode {
	MODE_UNDEFINED = 0,
	MODE_RUN,
};

static const char * const my_usage[] = {
	"git odb--daemon --run [<options>]",
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
	OPT_CMDMODE('r', "run", &my_args.mode,
		    N_("run the ODB daemon in the foreground"), MODE_RUN),

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
};

static struct my_odb_ipc_state *my_state;

static ipc_server_application_cb odb_cb;

static int odb_cb(void *data, const char *command,
		  ipc_server_reply_cb *reply_cb,
		  struct ipc_server_reply_data *reply_data)
{
	struct my_odb_ipc_state *state = data;

	assert(state == my_state);

	// TODO respond to request from client.

	return 0;
}

/*
 * Use the simple SYNC interface, start the IPC thread pool, and
 * block the calling thread until it shuts down.
 */
static int do_ipc(void)
{
	int ret;
	const char *path = odb_over_ipc__get_path();

	struct ipc_server_opts ipc_opts = {
		.nr_threads = my_args.nr_ipc_threads,
	};

	ret = ipc_server_run(path, &ipc_opts, odb_cb, my_state);

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

	// TODO Create mutexes and locks
	//
	// TODO Load up the packfiles
	//
	// TODO Consider starting a thread to watch for new/deleted packfiles
	// TODO and update the in-memory database.

	ret = do_ipc();

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

	default:
		die(_("Unhandled command mode"));
	}
}
#endif /* SUPPORTS_SIMPLE_IPC */
