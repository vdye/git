/*
 * GIT - the stupid content tracker
 *
 * Copyright (c) Junio C Hamano, 2006, 2009
 */
#include "builtin.h"
#include "gettext.h"
#include "hex.h"
#include "index-info.h"
#include "quote.h"
#include "read-cache-ll.h"
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
			   struct tree_entry_array *arr, int literally)
{
	struct tree_entry *ent;
	size_t len = strlen(path);

	if (literally) {
		FLEX_ALLOC_MEM(ent, name, path, len);
	} else {
		/* Normalize and validate entry path */
		if (S_ISDIR(mode)) {
			while(len > 0 && is_dir_sep(path[len - 1]))
				len--;
		}
		FLEX_ALLOC_MEM(ent, name, path, len);

		if (!verify_path(ent->name, mode))
			die(_("invalid path '%s'"), path);
		if (strchr(ent->name, '/'))
			die("path %s contains slash", path);
	}

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

static void write_tree_literally(struct tree_entry_array *arr,
				 struct object_id *oid)
{
	struct strbuf buf;
	size_t size = 0;

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
	"git mktree [-z] [--missing] [--literally] [--batch]",
	NULL
};

struct mktree_line_data {
	struct tree_entry_array *arr;
	int allow_missing;
	int literally;
};

static int mktree_line(unsigned int mode, struct object_id *oid,
		       enum object_type obj_type, int stage,
		       const char *path, void *cbdata)
{
	struct mktree_line_data *data = cbdata;
	enum object_type mode_type = object_type(mode);

	if (stage)
		die(_("path '%s' is unmerged"), path);

	if (obj_type != OBJ_ANY && mode_type != obj_type)
		die("object type (%s) doesn't match mode type (%s)",
		    type_name(obj_type), type_name(mode_type));

	if (!S_ISGITLINK(mode)) {
		struct object_info oi = OBJECT_INFO_INIT;
		enum object_type parsed_obj_type;
		unsigned int flags = OBJECT_INFO_LOOKUP_REPLACE |
				     OBJECT_INFO_QUICK |
				     OBJECT_INFO_SKIP_FETCH_OBJECT;

		oi.typep = &parsed_obj_type;

		if (oid_object_info_extended(the_repository, oid, &oi, flags) < 0) {
			/*
			 * If the object is missing and we aren't explicitly
			 * allowing missing objects, die(). Otherwise, continue
			 * without error.
			 */
			if (!data->allow_missing)
				die("entry '%s' object %s is unavailable", path,
				    oid_to_hex(oid));
		} else if (parsed_obj_type != mode_type) {
			/*
			 * The object exists but is of the wrong type.
			 * This is a problem regardless of allow_missing
			 * because the new tree entry will never be correct.
			 */
			die("entry '%s' object %s is a %s but specified type was (%s)",
			    path, oid_to_hex(oid), type_name(parsed_obj_type),
			    type_name(mode_type));
		}
	}

	append_to_tree(mode, oid, path, data->arr, data->literally);
	return 0;
}

int cmd_mktree(int ac, const char **av, const char *prefix)
{
	struct object_id oid;
	int nul_term_line = 0;
	int is_batch_mode = 0;
	struct tree_entry_array arr = { 0 };
	struct mktree_line_data mktree_line_data = { .arr = &arr };
	struct strbuf line = STRBUF_INIT;
	int ret;

	const struct option option[] = {
		OPT_BOOL('z', NULL, &nul_term_line, N_("input is NUL terminated")),
		OPT_BOOL(0, "missing", &mktree_line_data.allow_missing, N_("allow missing objects")),
		OPT_BOOL(0, "literally", &mktree_line_data.literally,
			 N_("do not sort, deduplicate, or validate paths of tree entries")),
		OPT_BOOL(0, "batch", &is_batch_mode, N_("allow creation of more than one tree")),
		OPT_END()
	};

	ac = parse_options(ac, av, prefix, option, mktree_usage, 0);

	do {
		ret = read_index_info(nul_term_line, mktree_line, &mktree_line_data, &line);
		if (ret < 0)
			break;

		if (ret == INDEX_INFO_UNRECOGNIZED_LINE) {
			if (line.len)
				die("input format error: %s", line.buf);
			else if (!is_batch_mode)
				/* empty lines denote tree boundaries in batch mode */
				die("input format error: (blank line only valid in batch mode)");
		}

		if (is_batch_mode && !ret && arr.nr < 1) {
			/*
			 * Execution gets here if the last tree entry is terminated with a
			 * new-line.  The final new-line has been made optional to be
			 * consistent with the original non-batch behaviour of mktree.
			 */
			; /* skip creating an empty tree */
		} else {
			if (mktree_line_data.literally)
				write_tree_literally(&arr, &oid);
			else
				write_tree(&arr, &oid);
			puts(oid_to_hex(&oid));
			fflush(stdout);
		}
		tree_entry_array_clear(&arr, 1); /* reset tree entry buffer for re-use in batch mode */
	} while (ret > 0);

	strbuf_release(&line);
	tree_entry_array_release(&arr, 1);
	return !!ret;
}
