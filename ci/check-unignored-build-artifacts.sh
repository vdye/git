#!/bin/sh
#
# Check whether the build created anything not in our .gitignore
#

. ${0%/*}/lib.sh

check_unignored_build_artifacts
