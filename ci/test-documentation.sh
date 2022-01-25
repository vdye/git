#!/bin/sh
#
# Perform sanity checks on "make doc" output and built documentation
#

. ${0%/*}/lib.sh

generator=$1

filter_log () {
	sed -e '/^GIT_VERSION = /d' \
	    -e "/constant Gem::ConfigMap is deprecated/d" \
	    -e '/^    \* new asciidoc flags$/d' \
	    -e '/stripped namespace before processing/d' \
	    -e '/Attributed.*IDs for element/d' \
	    "$1"
}

cat stderr.raw
filter_log stderr.raw >stderr.log
test ! -s stderr.log
test -s Documentation/git.html
if test "$generator" = "Asciidoctor"
then
	test -s Documentation/git.xml
	test -s Documentation/git.1
fi
grep "<meta name=\"generator\" content=\"$generator " Documentation/git.html

rm -f stdout.log stderr.log stderr.raw
