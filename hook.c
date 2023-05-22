#include "git-compat-util.h"
#include "environment.h"
#include "advice.h"
#include "gettext.h"
#include "hook.h"
#include "path.h"
#include "run-command.h"
#include "config.h"
#include "strbuf.h"
#include "setup.h"

static int early_hooks_path_config(const char *var, const char *value, void *data)
{
	if (!strcmp(var, "core.hookspath"))
		return git_config_pathname((const char **)data, var, value);

	return 0;
}

/* Discover the hook before setup_git_directory() was called */
static const char *hook_path_early(const char *name, struct strbuf *result)
{
	static struct strbuf hooks_dir = STRBUF_INIT;
	static int initialized;

	if (initialized < 0)
		return NULL;

	if (!initialized) {
		struct strbuf gitdir = STRBUF_INIT, commondir = STRBUF_INIT;
		const char *early_hooks_dir = NULL;

		if (discover_git_directory(&commondir, &gitdir) < 0) {
			initialized = -1;
			return NULL;
		}

		read_early_config(early_hooks_path_config, &early_hooks_dir);
		if (!early_hooks_dir)
			strbuf_addf(&hooks_dir, "%s/hooks/", commondir.buf);
		else {
			strbuf_add_absolute_path(&hooks_dir, early_hooks_dir);
			free((void *)early_hooks_dir);
			strbuf_addch(&hooks_dir, '/');
		}

		strbuf_release(&gitdir);
		strbuf_release(&commondir);

		initialized = 1;
	}

	strbuf_addf(result, "%s%s", hooks_dir.buf, name);
	return result->buf;
}

const char *find_hook(const char *name)
{
	static struct strbuf path = STRBUF_INIT;

	strbuf_reset(&path);
	if (have_git_dir()) {
		static int forced_config;

		if (!forced_config) {
			if (!git_hooks_path) {
				git_config_get_pathname("core.hookspath",
							&git_hooks_path);
				UNLEAK(git_hooks_path);
			}
			forced_config = 1;
		}

		strbuf_git_path(&path, "hooks/%s", name);
	} else if (!hook_path_early(name, &path))
		return NULL;

	if (access(path.buf, X_OK) < 0) {
		int err = errno;

#ifdef STRIP_EXTENSION
		strbuf_addstr(&path, STRIP_EXTENSION);
		if (access(path.buf, X_OK) >= 0)
			return path.buf;
		if (errno == EACCES)
			err = errno;
#endif

		if (err == EACCES && advice_enabled(ADVICE_IGNORED_HOOK)) {
			static struct string_list advise_given = STRING_LIST_INIT_DUP;

			if (!string_list_lookup(&advise_given, name)) {
				string_list_insert(&advise_given, name);
				advise(_("The '%s' hook was ignored because "
					 "it's not set as executable.\n"
					 "You can disable this warning with "
					 "`git config advice.ignoredHook false`."),
				       path.buf);
			}
		}
		return NULL;
	}
	return path.buf;
}

int hook_exists(const char *name)
{
	return !!find_hook(name);
}

static int pick_next_hook(struct child_process *cp,
			  struct strbuf *out UNUSED,
			  void *pp_cb,
			  void **pp_task_cb UNUSED)
{
	struct hook_cb_data *hook_cb = pp_cb;
	const char *hook_path = hook_cb->hook_path;

	if (!hook_path)
		return 0;

	cp->no_stdin = 1;
	strvec_pushv(&cp->env, hook_cb->options->env.v);
	/* reopen the file for stdin; run_command closes it. */
	if (hook_cb->options->path_to_stdin) {
		cp->no_stdin = 0;
		cp->in = xopen(hook_cb->options->path_to_stdin, O_RDONLY);
	}
	cp->stdout_to_stderr = 1;
	cp->trace2_hook_name = hook_cb->hook_name;
	cp->dir = hook_cb->options->dir;

	strvec_push(&cp->args, hook_path);
	strvec_pushv(&cp->args, hook_cb->options->args.v);

	/*
	 * This pick_next_hook() will be called again, we're only
	 * running one hook, so indicate that no more work will be
	 * done.
	 */
	hook_cb->hook_path = NULL;

	return 1;
}

static int notify_start_failure(struct strbuf *out UNUSED,
				void *pp_cb,
				void *pp_task_cp UNUSED)
{
	struct hook_cb_data *hook_cb = pp_cb;

	hook_cb->rc |= 1;

	return 1;
}

static int notify_hook_finished(int result,
				struct strbuf *out UNUSED,
				void *pp_cb,
				void *pp_task_cb UNUSED)
{
	struct hook_cb_data *hook_cb = pp_cb;
	struct run_hooks_opt *opt = hook_cb->options;

	hook_cb->rc |= result;

	if (opt->invoked_hook)
		*opt->invoked_hook = 1;

	return 0;
}

static void run_hooks_opt_clear(struct run_hooks_opt *options)
{
	strvec_clear(&options->env);
	strvec_clear(&options->args);
}

int run_hooks_opt(const char *hook_name, struct run_hooks_opt *options)
{
	struct strbuf abs_path = STRBUF_INIT;
	struct hook_cb_data cb_data = {
		.rc = 0,
		.hook_name = hook_name,
		.options = options,
	};
	const char *hook_path = find_hook(hook_name);
	int ret = 0;
	const struct run_process_parallel_opts opts = {
		.tr2_category = "hook",
		.tr2_label = hook_name,

		.processes = 1,
		.ungroup = 1,

		.get_next_task = pick_next_hook,
		.start_failure = notify_start_failure,
		.task_finished = notify_hook_finished,

		.data = &cb_data,
	};

	/*
	 * Backwards compatibility hack in VFS for Git: when originally
	 * introduced (and used!), it was called `post-indexchanged`, but this
	 * name was changed during the review on the Git mailing list.
	 *
	 * Therefore, when the `post-index-change` hook is not found, let's
	 * look for a hook with the old name (which would be found in case of
	 * already-existing checkouts).
	 */
	if (!hook_path && !strcmp(hook_name, "post-index-change"))
		hook_path = find_hook("post-indexchanged");

	if (!options)
		BUG("a struct run_hooks_opt must be provided to run_hooks");

	if (options->invoked_hook)
		*options->invoked_hook = 0;

	if (!hook_path && !options->error_if_missing)
		goto cleanup;

	if (!hook_path) {
		ret = error("cannot find a hook named %s", hook_name);
		goto cleanup;
	}

	cb_data.hook_path = hook_path;
	if (options->dir) {
		strbuf_add_absolute_path(&abs_path, hook_path);
		cb_data.hook_path = abs_path.buf;
	}

	run_processes_parallel(&opts);
	ret = cb_data.rc;
cleanup:
	strbuf_release(&abs_path);
	run_hooks_opt_clear(options);
	return ret;
}

int run_hooks(const char *hook_name)
{
	struct run_hooks_opt opt = RUN_HOOKS_OPT_INIT;

	return run_hooks_opt(hook_name, &opt);
}

int run_hooks_l(const char *hook_name, ...)
{
	struct run_hooks_opt opt = RUN_HOOKS_OPT_INIT;
	va_list ap;
	const char *arg;

	va_start(ap, hook_name);
	while ((arg = va_arg(ap, const char *)))
		strvec_push(&opt.args, arg);
	va_end(ap);

	return run_hooks_opt(hook_name, &opt);
}
