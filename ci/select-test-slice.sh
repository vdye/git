#!/bin/sh
#
# Select a portion of the tests for testing Git in parallel
#

set -ex

tests=$(echo $(cd t && ./helper/test-tool path-utils slice-tests "$1" "$2" \
	t[0-9]*.sh))
echo T="$tests" >>$GITHUB_ENV
