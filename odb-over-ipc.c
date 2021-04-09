#include "git-compat-util.h"
#include "path.h"
#include "simple-ipc.h"
#include "odb-over-ipc.h"

int odb_over_ipc__is_supported(void)
{
#ifdef SUPPORTS_SIMPLE_IPC
	return 1;
#else
	return 0;
#endif
}

#ifdef SUPPORTS_SIMPLE_IPC
/*
 * We claim "<gitdir>/odb-over-ipc" as the name of the Unix Domain Socket
 * that we will use on Unix.  And something based on this unique string
 * in the Named Pipe File System on Windows.  So we don't need a command
 * line argument for this.
 */
GIT_PATH_FUNC(odb_over_ipc__get_path, "odb-over-ipc")

enum ipc_active_state odb_over_ipc__get_state(void)
{
	return ipc_get_active_state(odb_over_ipc__get_path());
}

int odb_over_ipc__command(const char *command, struct strbuf *answer)
{
	// TODO
	return -1;
}

#endif /* SUPPORTS_SIMPLE_IPC */
