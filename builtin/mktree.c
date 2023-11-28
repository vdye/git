/*
 * GIT - the stupid content tracker
 *
 * Copyright (c) Junio C Hamano, 2006, 2009
 */
#include "builtin.h"
#include "gettext.h"
#include "hex.h"
#include "quote.h"
#include "strbuf.h"
#include "tree.h"
#include "parse-options.h"
#include "object-store-ll.h"

struct tree_entry {
	unsigned mode;
	struct object_id oid;
	int len;
	char name[FLEX_ARRAY];
};

struct tree_entry_array {
	size_t nr, alloc;
	struct tree_entry **entries;
};

static void tree_entry_array_push(struct tree_entry_array *arr, struct tree_entry *ent)
{
	ALLOC_GROW(arr->entries, arr->nr + 1, arr->alloc);
	arr->entries[arr->nr++] = ent;
}

static void tree_entry_array_clear(struct tree_entry_array *arr, int free_entries)
{
	if (free_entries) {
		for (size_t i = 0; i < arr->nr; i++)
			FREE_AND_NULL(arr->entries[i]);
	}
	arr->nr = 0;
}

static void tree_entry_array_release(struct tree_entry_array *arr, int free_entries)
{
	tree_entry_array_clear(arr, free_entries);
	FREE_AND_NULL(arr->entries);
	arr->alloc = 0;
}

static void append_to_tree(unsigned mode, struct object_id *oid, const char *path,
			   struct tree_entry_array *arr)
{
	struct tree_entry *ent;
	size_t len = strlen(path);
	if (strchr(path, '/'))
		die("path %s contains slash", path);

	FLEX_ALLOC_MEM(ent, name, path, len);
	ent->mode = mode;
	ent->len = len;
	oidcpy(&ent->oid, oid);

	tree_entry_array_push(arr, ent);
}

static int ent_compare(const void *a_, const void *b_)
{
	struct tree_entry *a = *(struct tree_entry **)a_;
	struct tree_entry *b = *(struct tree_entry **)b_;
	return base_name_compare(a->name, a->len, a->mode,
				 b->name, b->len, b->mode);
}

static void write_tree(struct tree_entry_array *arr, struct object_id *oid)
{
	struct strbuf buf;
	size_t size = 0;

	QSORT(arr->entries, arr->nr, ent_compare);
	for (size_t i = 0; i < arr->nr; i++)
		size += 32 + arr->entries[i]->len;

	strbuf_init(&buf, size);
	for (size_t i = 0; i < arr->nr; i++) {
		struct tree_entry *ent = arr->entries[i];
		strbuf_addf(&buf, "%o %s%c", ent->mode, ent->name, '\0');
		strbuf_add(&buf, ent->oid.hash, the_hash_algo->rawsz);
	}

	write_object_file(buf.buf, buf.len, OBJ_TREE, oid);
	strbuf_release(&buf);
}

static const char *mktree_usage[] = {
	"git mktree [-z] [--missing] [--batch]",
	NULL
};

static void mktree_line(char *buf, int nul_term_line, int allow_missing,
			struct tree_entry_array *arr)
{
	char *ptr, *ntr;
	const char *p;
	unsigned mode;
	enum object_type mode_type; /* object type derived from mode */
	enum object_type obj_type; /* object type derived from sha */
	struct object_info oi = OBJECT_INFO_INIT;
	char *path, *to_free = NULL;
	struct object_id oid;

	ptr = buf;
	/*
	 * Read non-recursive ls-tree output format:
	 *     mode SP type SP sha1 TAB name
	 */
	mode = strtoul(ptr, &ntr, 8);
	if (ptr == ntr || !ntr || *ntr != ' ')
		die("input format error: %s", buf);
	ptr = ntr + 1; /* type */
	ntr = strchr(ptr, ' ');
	if (!ntr || parse_oid_hex(ntr + 1, &oid, &p) ||
	    *p != '\t')
		die("input format error: %s", buf);

	/* It is perfectly normal if we do not have a commit from a submodule */
	if (S_ISGITLINK(mode))
		allow_missing = 1;


	*ntr++ = 0; /* now at the beginning of SHA1 */

	path = (char *)p + 1;  /* at the beginning of name */
	if (!nul_term_line && path[0] == '"') {
		struct strbuf p_uq = STRBUF_INIT;
		if (unquote_c_style(&p_uq, path, NULL))
			die("invalid quoting");
		path = to_free = strbuf_detach(&p_uq, NULL);
	}

	/*
	 * Object type is redundantly derivable three ways.
	 * These should all agree.
	 */
	mode_type = object_type(mode);
	if (mode_type != type_from_string(ptr)) {
		die("entry '%s' object type (%s) doesn't match mode type (%s)",
			path, ptr, type_name(mode_type));
	}

	/* Check the type of object identified by oid without fetching objects */
	oi.typep = &obj_type;
	if (oid_object_info_extended(the_repository, &oid, &oi,
				     OBJECT_INFO_LOOKUP_REPLACE |
				     OBJECT_INFO_QUICK |
				     OBJECT_INFO_SKIP_FETCH_OBJECT) < 0)
		obj_type = -1;

	if (obj_type < 0) {
		if (allow_missing) {
			; /* no problem - missing objects are presumed to be of the right type */
		} else {
			die("entry '%s' object %s is unavailable", path, oid_to_hex(&oid));
		}
	} else {
		if (obj_type != mode_type) {
			/*
			 * The object exists but is of the wrong type.
			 * This is a problem regardless of allow_missing
			 * because the new tree entry will never be correct.
			 */
			die("entry '%s' object %s is a %s but specified type was (%s)",
				path, oid_to_hex(&oid), type_name(obj_type), type_name(mode_type));
		}
	}

	append_to_tree(mode, &oid, path, arr);
	free(to_free);
}

int cmd_mktree(int ac, const char **av, const char *prefix)
{
	struct strbuf sb = STRBUF_INIT;
	struct object_id oid;
	int nul_term_line = 0;
	int allow_missing = 0;
	int is_batch_mode = 0;
	int got_eof = 0;
	struct tree_entry_array arr = { 0 };
	strbuf_getline_fn getline_fn;

	const struct option option[] = {
		OPT_BOOL('z', NULL, &nul_term_line, N_("input is NUL terminated")),
		OPT_BOOL(0, "missing", &allow_missing, N_("allow missing objects")),
		OPT_BOOL(0, "batch", &is_batch_mode, N_("allow creation of more than one tree")),
		OPT_END()
	};

	ac = parse_options(ac, av, prefix, option, mktree_usage, 0);
	getline_fn = nul_term_line ? strbuf_getline_nul : strbuf_getline_lf;

	while (!got_eof) {
		while (1) {
			if (getline_fn(&sb, stdin) == EOF) {
				got_eof = 1;
				break;
			}
			if (sb.buf[0] == '\0') {
				/* empty lines denote tree boundaries in batch mode */
				if (is_batch_mode)
					break;
				die("input format error: (blank line only valid in batch mode)");
			}
			mktree_line(sb.buf, nul_term_line, allow_missing, &arr);
		}
		if (is_batch_mode && got_eof && arr.nr < 1) {
			/*
			 * Execution gets here if the last tree entry is terminated with a
			 * new-line.  The final new-line has been made optional to be
			 * consistent with the original non-batch behaviour of mktree.
			 */
			; /* skip creating an empty tree */
		} else {
			write_tree(&arr, &oid);
			puts(oid_to_hex(&oid));
			fflush(stdout);
		}
		tree_entry_array_clear(&arr, 1); /* reset tree entry buffer for re-use in batch mode */
	}

	tree_entry_array_release(&arr, 1);
	strbuf_release(&sb);
	return 0;
}
