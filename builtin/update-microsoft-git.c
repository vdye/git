#include "builtin.h"
#include "repository.h"
#include "parse-options.h"
#include "run-command.h"
#include "strvec.h"

#if defined(GIT_WINDOWS_NATIVE)
/*
 * On Windows, run 'git update-git-for-windows' which
 * is installed by the installer, based on the script
 * in git-for-windows/build-extra.
 */
static int platform_specific_upgrade(void)
{
	int res;
	struct strvec args = STRVEC_INIT;

	strvec_push(&args, "git-update-git-for-windows");
	res = run_command_v_opt(args.v, 0);
	strvec_clear(&args);
	return res;
}
#elif defined(__APPLE__)
/*
 * On macOS, we expect the user to have the microsoft-git
 * cask installed via Homebrew. We check using these
 * commands:
 *
 * 1. 'brew update' to get latest versions.
 * 2. 'brew upgrade --cask microsoft-git' to get the
 *    latest version.
 */
static int platform_specific_upgrade(void)
{
	int res;
	struct strvec args = STRVEC_INIT;

	printf("Updating Homebrew with 'brew update'\n");

	strvec_pushl(&args, "brew", "update", NULL);
	res = run_command_v_opt(args.v, 0);
	strvec_clear(&args);

	if (res) {
		error(_("'brew update' failed; is brew installed?"));
		return 1;
	}

	printf("Upgrading microsoft-git with 'brew upgrade --cask microsoft-git'\n");
	strvec_pushl(&args, "brew", "upgrade", "--cask", "microsoft-git", NULL);
	res = run_command_v_opt(args.v, 0);
	strvec_clear(&args);

	return res;
}
#else
static int platform_specific_upgrade(void)
{
	error(_("update-microsoft-git is not supported on this platform"));
	return 1;
}
#endif

static const char builtin_update_microsoft_git_usage[] =
	N_("git update-microsoft-git");

int cmd_update_microsoft_git(int argc, const char **argv, const char *prefix)
{
	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage(builtin_update_microsoft_git_usage);

	return platform_specific_upgrade();
}
