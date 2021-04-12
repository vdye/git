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
	struct ipc_client_connection *connection = NULL;
	struct ipc_client_connect_options options
		= IPC_CLIENT_CONNECT_OPTIONS_INIT;
	int ret;
	enum ipc_active_state state;

	strbuf_reset(answer);

	options.wait_if_busy = 1;
	options.wait_if_not_found = 0;

	state = ipc_client_try_connect(odb_over_ipc__get_path(), &options,
				       &connection);
	if (state != IPC_STATE__LISTENING) {
		die("odb--daemon is not running");
		return -1;
	}

	ret = ipc_client_send_command_to_connection(connection, command, answer);
	ipc_client_close_connection(connection);

	if (ret == -1) {
		die("could not send '%s' command to odb--daemon",
		    command);
		return -1;
	}

	return 0;
}

#endif /* SUPPORTS_SIMPLE_IPC */
