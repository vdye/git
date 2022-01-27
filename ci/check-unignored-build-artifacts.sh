#!/bin/sh
#
# Check whether the build created anything not in our .gitignore
#

set -ex

. ${0%/*}/lib-tput.sh

check_unignored_build_artifacts ()
{
	! git ls-files --other --exclude-standard --error-unmatch \
		-- ':/*' 2>/dev/null ||
	{
		echo "$(tput setaf 1)error: found unignored build artifacts$(tput sgr0)"
		false
	}
}

check_unignored_build_artifacts
