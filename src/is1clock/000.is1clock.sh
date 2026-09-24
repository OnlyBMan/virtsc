#!/bin/sh
# Keep the guest clock in step with the host. /etc/rc runs this before init
# starts the ttys, so the TSC rate is fixed and the first sync done before
# /twc/util/startup.sh brings up X and renderd. See is1clock.c for why.
case "$1" in
start)
    if [ -x /usr/local/sbin/is1clock ]; then
        /usr/local/sbin/is1clock -f && /usr/local/sbin/is1clock -d
    fi
    ;;
esac
exit 0
