#!/bin/sh

test_description='sparse checkout with submodules'

GIT_TEST_SPLIT_INDEX=0
GIT_TEST_SPARSE_INDEX=
TEST_NO_CREATE_REPO=1

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-sparse-checkout.sh

# Because 'init_repos' cd's into 'test-repo', it & the test that follows it need
# to be run in a subshell.
init_repos () {
	rm -rf test-repo &&
	cp -r initial-repo test-repo &&

	# refresh the index to avoid "not up-to-date" issues caused by changed
	# file creation time
	git -C test-repo update-index --refresh &&
	git -C test-repo/sub1 update-index --refresh &&
	git -C test-repo/deep/sub2 update-index --refresh &&
	git -C test-repo/folder1/sub3 update-index --refresh &&

	cd test-repo
}

test_expect_success 'setup' '
	test_config_global protocol.file.allow always &&

	git init submodule-remote &&
	(
		cd submodule-remote &&
		populate_test_repo
	) &&

	git init initial-repo &&
	(
		cd initial-repo &&

		# Set up the submodules
		git submodule add ../submodule-remote ./sub1 &&
		git submodule add ../submodule-remote ./deep/sub2  &&
		git submodule add ../submodule-remote ./folder1/sub3 &&

		# Set up the super repo
		populate_test_repo &&
		git submodule update
	)
'

# NEEDSWORK: when running `grep` in the superproject with --recurse-submodules,
# Git expands the index of the submodules unexpectedly. Even though `grep`
# builtin is marked as "command_requires_full_index = 0", this config is only
# useful for the superproject. Namely, the submodules have their own configs,
# which are _not_ populated by the one-time sparse-index feature switch.
test_expect_failure 'grep within submodules is not expanded' '
(
	init_repos &&

	# Setup both super project and submodule to use sparse index
	git sparse-checkout set --sparse-index deep &&
	git -C deep/sub2 sparse-checkout set --sparse-index deep &&

	GIT_TRACE2_EVENT="$(pwd)/trace2.txt" \
		git grep --cached --recurse-submodules a -- "deep/sub2/*" &&
	test_region ! index ensure_full_index trace2.txt
)
'

# NEEDSWORK: this test is not actually testing the code. The design purpose
# of this test is to verify the grep result when the submodules are using a
# sparse-index. Namely, we want "folder2/" as a tree (a sparse directory); but
# because of the index expansion, we are now grepping the "folder2/a" blob.
# Because of the problem stated above 'grep within submodules is not expanded',
# we don't have the ideal test environment yet.
test_expect_success 'grep sparse directory within submodules' '
(
	init_repos &&

	# Configure only one submodule to use sparse index
	git -C sub1 sparse-checkout set --sparse-index &&

	cat >expect <<-\EOF &&
	deep/sub2/folder2/a:a
	folder1/sub3/folder2/a:a
	sub1/folder2/a:a
	EOF

	git grep --cached --recurse-submodules a -- "*/folder2/*" >actual &&
	test_cmp actual expect
)
'

test_expect_failure 'ls-files: sparse index super project, sparse index submodule' '
(
	init_repos &&

	# Give the super project & submodule different patterns
	git sparse-checkout set --sparse-index deep &&
	git -C deep/sub2 sparse-checkout set --sparse-index &&

	git -C deep/sub2 ls-files --sparse -t >expect &&

	git ls-files --sparse -t --recurse-submodules -- deep/sub2 \
	| sed -e "s/deep\/sub2\///" >actual &&

	test_cmp expect actual
)
'

test_expect_failure 'ls-files: sparse-checkout super project, sparse index submodule' '
(
	init_repos &&

	# Give the super project & submodule different patterns
	git sparse-checkout set --no-sparse-index deep &&
	git -C sub1 sparse-checkout set --sparse-index &&

	git -C sub1 ls-files --sparse -t >expect &&

	git ls-files --sparse -t --recurse-submodules -- sub1 \
	| sed -e "s/sub1\///" >actual &&

	test_cmp expect actual
)
'

test_expect_failure 'ls-files: non-sparse super project, sparse index submodule' '
(
	init_repos &&
	git -C sub1 sparse-checkout set --sparse-index deep &&

	git -C sub1 ls-files --sparse -t >expect &&

	git ls-files --sparse -t --recurse-submodules -- sub1 \
	| sed -e "s/sub1\///" >actual &&

	test_cmp expect actual
)
'

test_expect_success 'ls-files: sparse index super project, sparse-checkout submodule' '
(
	init_repos &&
	git sparse-checkout set --sparse-index deep &&
	git -C sub1 sparse-checkout set --no-sparse-index &&

	git -C deep/sub2 ls-files --sparse -t >expect &&

	git ls-files --sparse -t --recurse-submodules -- deep/sub2 \
	| sed -e "s/deep\/sub2\///" >actual &&

	test_cmp expect actual
)
'

test_expect_success 'ls-files: sparse index super project, non-sparse submodule' '
(
	init_repos &&
	git sparse-checkout set --sparse-index deep &&

	git -C deep/sub2 ls-files --sparse -t >expect &&

	git ls-files --sparse -t --recurse-submodules -- deep/sub2 \
	| sed -e "s/deep\/sub2\///" >actual &&

	test_cmp expect actual
)
'

test_expect_success 'file cannot be added inside SKIP_WORKTREE submodule' '
(
	init_repos &&
	git sparse-checkout set deep &&

	mkdir -p folder1/sub3/folder2 &&
	echo b >folder1/sub3/folder2/b &&

	cat >expected <<-EOF &&
	fatal: Pathspec ${SQ}folder1/sub3/folder2/b${SQ} is in submodule ${SQ}folder1/sub3${SQ}
	EOF

	test_must_fail git add --sparse folder1/sub3/folder2/b 2>err &&
	test_cmp expected err
)
'

test_expect_failure 'SKIP_WORKTREE cleared within submodules' '
(
	init_repos &&
	git -C sub1 sparse-checkout set deep &&

	git ls-files --sparse -t --recurse-submodules -- sub1 >out &&
	grep "S sub1/folder1/a" out &&

	mkdir -p sub1/folder1 &&
	echo b >sub1/folder1/a &&

	git ls-files --sparse -t --recurse-submodules -- sub1 >out &&
	grep "H sub1/folder1/a" out
)
'

test_done
