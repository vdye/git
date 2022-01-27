#!/bin/sh
#
# Build and test Git
#

. ${0%/*}/lib.sh

export MAKE_TARGETS="all test"

case "$jobname" in
linux-gcc)
	setenv --test GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME main
	;;
linux-TEST-vars)
	setenv --test GIT_TEST_SPLIT_INDEX yes
	setenv --test GIT_TEST_MERGE_ALGORITHM recursive
	setenv --test GIT_TEST_FULL_IN_PACK_ARRAY true
	setenv --test GIT_TEST_OE_SIZE 10
	setenv --test GIT_TEST_OE_DELTA_SIZE 5
	setenv --test GIT_TEST_COMMIT_GRAPH 1
	setenv --test GIT_TEST_COMMIT_GRAPH_CHANGED_PATHS 1
	setenv --test GIT_TEST_MULTI_PACK_INDEX 1
	setenv --test GIT_TEST_MULTI_PACK_INDEX_WRITE_BITMAP 1
	setenv --test GIT_TEST_ADD_I_USE_BUILTIN 1
	setenv --test GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME master
	setenv --test GIT_TEST_WRITE_REV_INDEX 1
	setenv --test GIT_TEST_CHECKOUT_WORKERS 2
	;;
linux-clang)
	setenv --test GIT_TEST_DEFAULT_HASH sha1
	;;
linux-sha256)
	setenv --test GIT_TEST_DEFAULT_HASH sha256
	;;
pedantic)
	# Don't run the tests; we only care about whether Git can be
	# built.
	setenv --build DEVOPTS pedantic
	export MAKE_TARGETS=all
	;;
esac

# Any new "test" targets should not go after this "make", but should
# adjust $MAKE_TARGETS. Otherwise compilation-only targets above will
# start running tests.
make $MAKE_TARGETS
check_unignored_build_artifacts
