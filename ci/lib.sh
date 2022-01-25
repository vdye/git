#!/bin/sh

# Library of functions shared by all CI scripts

# Set 'exit on error' for all CI scripts to let the caller know that
# something went wrong.
# Set tracing executed commands, primarily setting environment variables
# and installing dependencies.
set -ex

# Starting assertions
if test -z "$jobname"
then
	echo "must set a CI jobname" >&2
	exit 1
fi

# Helper functions
setenv () {
	varmode=
	case "$1" in
	--*)
		varmode=$1
		shift
		;;
	esac

	key=$1
	val=$2
	shift 2

	if test -n "$GITHUB_ENV"
	then
		echo "$key=$val" >>"$GITHUB_ENV"
	else
		# For local debugging. Not used by the GitHub CI
		# itself.
		eval "export $key=\"$val\""
	fi
}

check_unignored_build_artifacts ()
{
	! git ls-files --other --exclude-standard --error-unmatch \
		-- ':/*' 2>/dev/null ||
	{
		echo "$(tput setaf 1)error: found unignored build artifacts$(tput sgr0)"
		false
	}
}

# GitHub Action doesn't set TERM, which is required by tput
setenv TERM ${TERM:-dumb}

# Clear MAKEFLAGS that may come from the outside world.
MAKEFLAGS=

if test "$GITHUB_ACTIONS" = "true"
then
	CI_TYPE=github-actions
	CC="${CC:-gcc}"

	setenv --test GIT_PROVE_OPTS "--timer --jobs 10"
	GIT_TEST_OPTS="--verbose-log -x"
	MAKEFLAGS="$MAKEFLAGS --jobs=10"
	test Windows != "$RUNNER_OS" ||
	GIT_TEST_OPTS="--no-chain-lint --no-bin-wrappers $GIT_TEST_OPTS"

	setenv --test GIT_TEST_OPTS "$GIT_TEST_OPTS"
else
	echo "Could not identify CI type" >&2
	env >&2
	exit 1
fi

setenv --build DEVELOPER 1
setenv --test DEFAULT_TEST_TARGET prove
setenv --test GIT_TEST_CLONE_2GB true
setenv --build SKIP_DASHED_BUILT_INS YesPlease

case "$runs_on_pool" in
ubuntu-latest)
	if test "$jobname" = "linux-gcc-default"
	then
		break
	fi

	if [ "$jobname" = linux-gcc ]
	then
		MAKEFLAGS="$MAKEFLAGS PYTHON_PATH=/usr/bin/python3"
	else
		MAKEFLAGS="$MAKEFLAGS PYTHON_PATH=/usr/bin/python2"
	fi

	setenv --test GIT_TEST_HTTPD true
	;;
macos-latest)
	if [ "$jobname" = osx-gcc ]
	then
		MAKEFLAGS="$MAKEFLAGS PYTHON_PATH=$(which python3)"
	else
		MAKEFLAGS="$MAKEFLAGS PYTHON_PATH=$(which python2)"
	fi
	;;
esac

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
	;;
linux32)
	CC=gcc
	;;
linux-musl)
	CC=gcc
	MAKEFLAGS="$MAKEFLAGS PYTHON_PATH=/usr/bin/python3 USE_LIBPCRE2=Yes"
	MAKEFLAGS="$MAKEFLAGS NO_REGEX=Yes ICONV_OMITS_BOM=Yes"
	MAKEFLAGS="$MAKEFLAGS GIT_TEST_UTF8_LOCALE=C.UTF-8"
	;;
linux-leaks)
	setenv --build SANITIZE leak
	setenv --test GIT_TEST_PASSING_SANITIZE_LEAK true
	;;
esac

setenv --all MAKEFLAGS "$MAKEFLAGS CC=${CC:-cc}"
