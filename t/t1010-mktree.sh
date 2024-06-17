#!/bin/sh

test_description='git mktree'

TEST_PASSES_SANITIZE_LEAK=true
. ./test-lib.sh

test_expect_success setup '
	for d in a a- a0
	do
		mkdir "$d" && echo "$d/one" >"$d/one" &&
		git add "$d" || return 1
	done &&
	echo zero >one &&
	git update-index --add --info-only one &&
	git write-tree --missing-ok >tree.missing &&
	git ls-tree $(cat tree.missing) >top.missing &&
	git ls-tree -r $(cat tree.missing) >all.missing &&
	echo one >one &&
	git add one &&
	git write-tree >tree &&
	git ls-tree $(cat tree) >top &&
	git ls-tree -r $(cat tree) >all &&
	test_tick &&
	git commit -q -m one &&
	H=$(git rev-parse HEAD) &&
	git update-index --add --cacheinfo 160000 $H sub &&
	test_tick &&
	git commit -q -m two &&
	git rev-parse HEAD^{tree} >tree.withsub &&
	git ls-tree HEAD >top.withsub &&
	git ls-tree -r HEAD >all.withsub
'

test_expect_success 'ls-tree piped to mktree (1)' '
	git mktree <top >actual &&
	test_cmp tree actual
'

test_expect_success 'ls-tree piped to mktree (2)' '
	git mktree <top.withsub >actual &&
	test_cmp tree.withsub actual
'

test_expect_success 'ls-tree output in wrong order given to mktree (1)' '
	perl -e "print reverse <>" <top |
	git mktree >actual &&
	test_cmp tree actual
'

test_expect_success 'ls-tree output in wrong order given to mktree (2)' '
	perl -e "print reverse <>" <top.withsub |
	git mktree >actual &&
	test_cmp tree.withsub actual
'

test_expect_success '--batch creates multiple trees' '
	cat top >multi-tree &&
	echo "" >>multi-tree &&
	cat top.withsub >>multi-tree &&

	cat tree >expect &&
	cat tree.withsub >>expect &&
	git mktree --batch <multi-tree >actual &&
	test_cmp expect actual
'

test_expect_success 'allow missing object with --missing' '
	git mktree --missing <top.missing >actual &&
	test_cmp tree.missing actual
'

test_expect_success 'mktree with invalid submodule OIDs' '
	for oid in "$(test_oid numeric)" "$(cat tree)"
	do
		printf "160000 commit $oid\tA\n" >in &&
		git mktree <in >tree.actual &&
		git ls-tree $(cat tree.actual) >actual &&
		test_cmp in actual || return 1
	done
'

test_expect_success 'mktree refuses to read ls-tree -r output (1)' '
	test_must_fail git mktree <all
'

test_expect_success 'mktree refuses to read ls-tree -r output (2)' '
	test_must_fail git mktree <all.withsub
'

test_expect_success 'mktree fails on malformed input' '
	# empty line without --batch
	echo "" |
	test_must_fail git mktree 2>err &&
	test_grep "blank line only valid in batch mode" err &&

	# bad whitespace
	printf "100644 blob $EMPTY_BLOB A" |
	test_must_fail git mktree 2>err &&
	test_grep "input format error" err &&

	# invalid type
	printf "100644 bad $EMPTY_BLOB\tA" |
	test_must_fail git mktree 2>err &&
	test_grep "invalid object type" err &&

	# invalid OID length
	printf "100755 blob abc123\tA" |
	test_must_fail git mktree 2>err &&
	test_grep "input format error" err &&

	# bad quoting
	printf "100644 blob $EMPTY_BLOB\t\"A" |
	test_must_fail git mktree 2>err &&
	test_grep "bad quoting of path name" err
'

test_expect_success 'mktree fails on mode mismatch' '
	tree_oid="$(cat tree)" &&

	# mode-type mismatch
	printf "100644 tree $tree_oid\tA" |
	test_must_fail git mktree 2>err &&
	test_grep "object type (tree) doesn${SQ}t match mode type (blob)" err &&

	# mode-object mismatch (no --missing)
	printf "100644 $tree_oid\tA" |
	test_must_fail git mktree 2>err &&
	test_grep "object $tree_oid is a tree but specified type was (blob)" err
'

test_done
