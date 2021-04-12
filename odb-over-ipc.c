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

static int is_daemon = 0;

void odb_over_ipc__set_is_daemon(void)
{
	is_daemon = 1;
}

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
		// error("odb--daemon is not running");
		return -1;
	}

	ret = ipc_client_send_command_to_connection(connection, command, answer);
	ipc_client_close_connection(connection);

	if (ret == -1) {
		error("could not send '%s' command to odb--daemon", command);
		return -1;
	}

	return 0;
}

int odb_over_ipc__get_oid(struct repository *r, const struct object_id *oid,
			  struct object_info *oi, unsigned flags)
{
	struct strbuf cmd = STRBUF_INIT;
	struct strbuf answer = STRBUF_INIT;
	int ret;

	if (is_daemon)
		return -1;

	if (r != the_repository)	// TODO not dealing with this
		return -1;

	/*
	 * If we are going to the trouble to ask the daemon for information on
	 * the object, always get all of the optional fields.  That is, don't
	 * worry with which fields within `oi` are populated on the request side.
	 */
	strbuf_addf(&cmd, "oid %s\n", oid_to_hex(oid));
	strbuf_addf(&cmd, "flags %"PRIuMAX"\n", (uintmax_t)flags);

	ret = odb_over_ipc__command(cmd.buf, &answer);

	strbuf_release(&cmd);
	if (ret)
		return ret;

	// TODO We expect either valid or error response....

	trace2_printf("get_oid: '%s'", answer.buf);

	// TODO actually use the result...

	strbuf_release(&answer);
	return -1;
}

#endif /* SUPPORTS_SIMPLE_IPC */
