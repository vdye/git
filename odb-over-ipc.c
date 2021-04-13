#include "git-compat-util.h"
#include "path.h"
#include "hex.h"
#include "object.h"
#include "object-store.h"
#include "trace2.h"
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
	char hex_buf[GIT_MAX_HEXSZ + 1];
	struct strbuf cmd = STRBUF_INIT;
	struct strbuf answer = STRBUF_INIT;
	struct strbuf headers = STRBUF_INIT;
	struct strbuf **lines = NULL;
	const char *sz;
	const char *ch_nul;
	const char *content;
	ssize_t content_len;
	int k;
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
	strbuf_addf(&cmd, "oid %s\n", oid_to_hex_r(hex_buf, oid));
	strbuf_addf(&cmd, "flags %"PRIuMAX"\n", (uintmax_t)flags);
	// TODO send another row to indicate whether we want the content buffer.

	ret = odb_over_ipc__command(cmd.buf, &answer);

	strbuf_release(&cmd);
	if (ret)
		return ret;

	if (!strncmp(answer.buf, "error", 5)) {
		trace2_printf("odb-over-ipc: failed for '%s'", oid_to_hex(oid));
		return -1;
	}

	if (!oi) {
		/*
		 * The caller doesn't care about the object itself;
		 * just whether it exists??
		 */
		goto done;
	}

	/* Find the divider between the headers and the content. */
	ch_nul = strchr(answer.buf, '\0');
	content = ch_nul + 1;
	content_len = &answer.buf[answer.len] - content;

	/*
	 * Extract the portion before the divider into another string so that
	 * we can split / parse it by lines.
	 */
	strbuf_add(&headers, answer.buf, (ch_nul - answer.buf));

	lines = strbuf_split_str(headers.buf, '\n', 0);
	strbuf_release(&headers);

	for (k = 0; lines[k]; k++) {
		strbuf_trim_trailing_newline(lines[k]);

		if (skip_prefix(lines[k]->buf, "oid ", &sz)) {
			assert(!strcmp(sz, oid_to_hex(oid)));
			continue;
		}

		if (skip_prefix(lines[k]->buf, "type ", &sz)) {
			enum object_type t = strtol(sz, NULL, 10);
			if (oi->typep)
				*(oi->typep) = t;
			if (oi->type_name)
				strbuf_addstr(oi->type_name, type_name(t));
			continue;
		}

		if (skip_prefix(lines[k]->buf, "size ", &sz)) {
			ssize_t size = strtoumax(sz, NULL, 10);
			assert(size == content_len);

			if (oi->sizep)
				*(oi->sizep) = size;
			continue;
		}

		if (skip_prefix(lines[k]->buf, "disk ", &sz)) {
			if (oi->disk_sizep)
				*(oi->disk_sizep) = strtoumax(sz, NULL, 10);
			continue;
		}

		// TODO do we really care about the delta-base ??
		if (skip_prefix(lines[k]->buf, "delta ", &sz)) {
			if (oi->delta_base_oid) {
				oidclr(oi->delta_base_oid);
				if (get_oid_hex(sz, oi->delta_base_oid)) {
					error("could not parse delta base in odb-over-ipc response");
					ret = -1;
					goto done;
				}
			}
			continue;
		}

		if (skip_prefix(lines[k]->buf, "whence ", &sz)) {
			oi->whence = strtol(sz, NULL, 10);
			continue;
		}

		// TODO The server does not send the contents of oi.u.packed.
		// TODO Do we care?

		BUG("unexpected line '%s' in OID response", lines[k]->buf);
	}

	if (oi->contentp)
		*oi->contentp = xmemdupz(content, content_len);

done:
	if (lines)
		strbuf_list_free(lines);
	strbuf_release(&answer);
	return ret;
}

#endif /* SUPPORTS_SIMPLE_IPC */
