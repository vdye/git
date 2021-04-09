#include "builtin.h"
#include "config.h"
#include "parse-options.h"
#include "simple-ipc.h"

static const char * const my_usage[] = {
	"git odb--daemon TBD...",
	NULL
};

static struct option my_options[] = {
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


int cmd_odb__daemon(int argc, const char **argv, const char *prefix)
{
	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(my_usage, my_options);

	git_config(git_default_config, NULL);

	argc = parse_options(argc, argv, prefix, my_options, my_usage, 0);

	die(_("Unhandled command mode"));
}
#endif /* SUPPORTS_SIMPLE_IPC */
