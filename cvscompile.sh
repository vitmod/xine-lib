#!/bin/sh
# Run this to generate all the initial Makefiles, etc.

rm -f config.cache

#m4_files="_xine.m4 alsa.m4 arts.m4 esd.m4 aa.m4 irixal.m4"
if test -d $srcdir/m4; then
    rm -f acinclude.m4
    for m4f in $srcdir/m4/*.m4; do
	cat $m4f >> acinclude.m4
    done
else
    echo "Directory 'm4' is missing."
    exit 1
fi

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

(test -f $srcdir/configure.in) || {
    echo -n "*** Error ***: Directory "\`$srcdir\'" does not look like the"
    echo " top-level directory"
    exit 1
}

. $srcdir/misc/autogen.sh

