#include "builtin.h"
#include "config.h"
#include "parse-options.h"
#include "simple-ipc.h"
#include "thread-utils.h"

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

static int try_run_daemon(void)
{
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

	default:
		die(_("Unhandled command mode"));
	}
}
#endif /* SUPPORTS_SIMPLE_IPC */
