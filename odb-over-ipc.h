#ifndef ODB_OVER_IPC_H
#define ODB_OVER_IPC_H

/*
 * Returns true if Simple IPC is supported on this platform.  This symbol
 * must always be visible and outside of the ifdef.
 */
int odb_over_ipc__is_supported(void);

#include "simple-ipc.h"

#ifdef SUPPORTS_SIMPLE_IPC

/*
 * Returns the pathname to the IPC named pipe or Unix domain socket
 * where a `git odb--daemon` process will listen.
 *
 * TODO Technically, this is a per repo value, since it needs to
 * TODO look at the ODB (both the `<gitdir>/objects` directory and
 * TODO ODB alternates).  So we should share a daemon between multiple
 * TODO worktrees.  Verify this.
 */
const char *odb_over_ipc__get_path(void);

/*
 * Try to determine whether there is a `git odb--daemon` process
 * listening on the official IPC named pipe / socket for the
 * current repo.
 */
enum ipc_active_state odb_over_ipc__get_state(void);

/*
 * Connect to an existing `git odb--daemon` process, send a command,
 * and receive a response.  If no daemon is running, this DOES NOT try
 * to start one.
 *
 * TODO If we can trust the code that creates/deletes packfiles, we
 * TODO might consider adding a command here to let that process tell
 * TODO the daemon to update the list of cached packfiles.
 *
 * TODO For simplicit during prototyping I am NOT going to
 * TODO auto-start one.  Revisit this later.
 */
int odb_over_ipc__command(const char *command, size_t command_len,
			  struct strbuf *answer);

struct odb_over_ipc__key
{
	char key[16];
};

struct odb_over_ipc__get_oid__request
{
	struct odb_over_ipc__key key;
	struct object_id oid;
	unsigned flags;
	unsigned want_content:1;
};

struct odb_over_ipc__get_oid__response
{
	struct odb_over_ipc__key key;
	struct object_id oid;
	struct object_id delta_base_oid;
	off_t disk_size;
	unsigned long size;
	unsigned whence; /* see struct object_info */
	enum object_type type;
};

struct odb_over_ipc__hash_object__request
{
	struct odb_over_ipc__key key;
	enum object_type type;
	unsigned flags;
	size_t content_size;
};

struct odb_over_ipc__hash_object__response
{
	struct odb_over_ipc__key key;
	struct object_id oid;
};

struct odb_over_ipc__get_parent__request
{
	struct odb_over_ipc__key key;
	int idx;
	int name_len;
};

struct odb_over_ipc__get_parent__response
{
	struct odb_over_ipc__key key;
	struct object_id oid;
};

struct odb_over_ipc__get_nth_ancestor__request
{
	struct odb_over_ipc__key key;
	int generation;
	int name_len;
};

struct odb_over_ipc__get_nth_ancestor__response
{
	struct odb_over_ipc__key key;
	struct object_id oid;
};

/*
 * Connect to an existing `git odb--daemon` process and ask it for
 * an object.  This is intended to be inserted into the client
 * near `oid_object_info_extended()`.
 *
 * Returns non-zero when the caller should use the traditional
 * method.
 */
struct object_info;
struct repository;

int odb_over_ipc__get_oid(struct repository *r, const struct object_id *oid,
			  struct object_info *oi, unsigned flags);


int odb_over_ipc__hash_object(struct repository *r, struct object_id *oid,
			      int fd, enum object_type type, unsigned flags);

int odb_over_ipc__get_parent(struct repository *r, const char *name, int len,
			     int idx, struct object_id *result);

int odb_over_ipc__get_nth_ancestor(struct repository *r, const char *name,
				   int len, int generation,
				   struct object_id *result);

/*
 * Explicitly shutdown IPC connection to the `git odb--daemon` process.
 * The connection is implicitly created upon the first request and we
 * use a keep-alive model to re-use it for subsequent requests.
 */
void odb_over_ipc__shutdown_keepalive_connection(void);

/*
 * Insurance to protect the daemon from calling ODB code and accidentally
 * falling into the client-side code and trying to connect to itself.
 */
void odb_over_ipc__set_is_daemon(void);

#endif /* SUPPORTS_SIMPLE_IPC */
#endif /* ODB_OVER_IPC_H */
