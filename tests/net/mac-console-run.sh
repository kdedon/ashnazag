#!/sbin/sh
/tests/runall "$@" >/tmp/macrun.log 2>&1
status=$?
sync
/tests/macdrain /tmp/otprobe.out
/tests/macdrain /tmp/macrun.log
exit "$status"
