#include "builtin.h"
#include "config.h"
#include "parse-options.h"
#include "fsmonitor.h"
#include "fsmonitor-ipc.h"
#include "fsmonitor--daemon.h"
#include "simple-ipc.h"
#include "khash.h"

static const char * const builtin_fsmonitor__daemon_usage[] = {
	N_("git fsmonitor--daemon --is-supported"),
	NULL
};

int cmd_fsmonitor__daemon(int argc, const char **argv, const char *prefix)
{
	enum daemon_mode {
		UNDEFINED_MODE,
		IS_SUPPORTED,
	} mode = UNDEFINED_MODE;

	struct option options[] = {
		OPT_CMDMODE(0, "is-supported", &mode,
			    N_("does this platform support fsmonitor--daemon"),
			    IS_SUPPORTED),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_fsmonitor__daemon_usage, options);

	git_config(git_default_config, NULL);

	argc = parse_options(argc, argv, prefix, options,
			     builtin_fsmonitor__daemon_usage, 0);

	if (mode == IS_SUPPORTED)
		return !fsmonitor_ipc__is_supported();

#ifdef HAVE_FSMONITOR_DAEMON_BACKEND
	switch (mode) {
	case UNDEFINED_MODE:
	default:
		die(_("Unhandled command mode %d"), mode);
	}
#else
	die(_("internal fsmonitor daemon not supported"));
#endif
}
