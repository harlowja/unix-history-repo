#!/bin/sh
#
if [ "`id -u`" != "0" ]; then
	echo "Sorry, this must be done as root."
	exit 1
fi
cat compat1x.?? | tar --unlink -xpzf - -C /
exit 0
