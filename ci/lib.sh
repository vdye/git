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
export TERM=${TERM:-dumb}

# Clear MAKEFLAGS that may come from the outside world.
export MAKEFLAGS=

if test -n "$SYSTEM_COLLECTIONURI" || test -n "$SYSTEM_TASKDEFINITIONSURI"
then
	CI_TYPE=azure-pipelines
	# We are running in Azure Pipelines
	CC="${CC:-gcc}"

	export GIT_PROVE_OPTS="--timer --jobs 10 --state=failed,slow,save"
	export GIT_TEST_OPTS="--verbose-log -x --write-junit-xml"
	MAKEFLAGS="$MAKEFLAGS --jobs=10"
	test Windows_NT != "$AGENT_OS" ||
	GIT_TEST_OPTS="--no-chain-lint --no-bin-wrappers $GIT_TEST_OPTS"
elif test true = "$GITHUB_ACTIONS"
then
	CI_TYPE=github-actions
	CC="${CC:-gcc}"

	export GIT_PROVE_OPTS="--timer --jobs 10"
	export GIT_TEST_OPTS="--verbose-log -x"
	MAKEFLAGS="$MAKEFLAGS --jobs=10"
	test Windows != "$RUNNER_OS" ||
	GIT_TEST_OPTS="--no-chain-lint --no-bin-wrappers $GIT_TEST_OPTS"
else
	echo "Could not identify CI type" >&2
	env >&2
	exit 1
fi

export DEVELOPER=1
export DEFAULT_TEST_TARGET=prove
export GIT_TEST_CLONE_2GB=true
export SKIP_DASHED_BUILT_INS=YesPlease

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

	export GIT_TEST_HTTPD=true

	# The Linux build installs the defined dependency versions below.
	# The OS X build installs much more recent versions, whichever
	# were recorded in the Homebrew database upon creating the OS X
	# image.
	# Keep that in mind when you encounter a broken OS X build!
	export LINUX_P4_VERSION="16.2"
	export LINUX_GIT_LFS_VERSION="1.5.2"

	P4_PATH="$HOME/custom/p4"
	GIT_LFS_PATH="$HOME/custom/git-lfs"
	export PATH="$GIT_LFS_PATH:$P4_PATH:$PATH"
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
	export SANITIZE=leak
	export GIT_TEST_PASSING_SANITIZE_LEAK=true
	;;
esac

MAKEFLAGS="$MAKEFLAGS CC=${CC:-cc}"
