#!/bin/sh
#
# Copyright 2016 Garrett D'Amore <garrett@damore.org>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

#
# This script is used to run uncrustify and report files that don't match.
# It looks for .c and .h files, located in ../src, and uses the config file
# uncrustify.cfg located in the same directory as this script. It only handles
# C language at this point.
#
mydir=`dirname $0`
srcdir=${mydir}/../src
failed=

uncrustify --version > /dev/null
if [ $? -ne 0 ]; then
	echo "Uncrustify not found.  Skipping checks."
	exit 0
fi

for file in `find ${srcdir} -name '*.[ch]' -print`
do
	uncrustify -c "${mydir}/uncrustify.cfg" -q -lC $file
	if [ $? -ne 0 ]; then
		echo "Cannot run uncrustify??" 1>&2
		exit 2
	fi
	if [ -t 1 ]; then
		colordiff -u $file $file.uncrustify
	else
		diff -u $file $file.uncrustify
	fi
	if [ $? -ne 0 ]; then
		failed=1
	fi
	rm ${file}.uncrustify
done
if [ -n "$failed" ]
then
	echo "Uncrustify differences found!" 1>&2
	# Sadly, there are different versions of Uncrustify, and they don't
	# seem to universally agree.  So let's not trigger a build error on
	# this -- but instead just emit it to standard output.
	exit 0
fi
