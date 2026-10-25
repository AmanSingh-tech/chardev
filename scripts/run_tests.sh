#!/usr/bin/env bash
# Build, load the module, run the tests (plain and under valgrind), show the
# kernel log for this run, and unload. Must run as root: sudo scripts/run_tests.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MOD=chardev
DEV=/dev/chardev
TEST="$ROOT/user/chardev_test"
RESULT=0

if [[ $EUID -ne 0 ]]; then
	echo "run as root: sudo $0" >&2
	exit 1
fi

if lsmod | grep -q "^$MOD "; then
	echo "$MOD is already loaded; unload it first (rmmod $MOD)" >&2
	exit 1
fi

# Build as the invoking user so build output isn't left owned by root.
BUILD_USER="${SUDO_USER:-root}"
echo "== building"
sudo -u "$BUILD_USER" make -C "$ROOT/kernel"
sudo -u "$BUILD_USER" make -C "$ROOT/user" debug

cleanup() {
	if lsmod | grep -q "^$MOD "; then
		echo "== unloading $MOD"
		rmmod "$MOD" || echo "rmmod failed" >&2
	fi
}
trap cleanup EXIT

# Mark the kernel log so we can print only this run's messages.
MARKER="run_tests.sh start $$ $(date +%s)"
echo "$MARKER" > /dev/kmsg

echo "== loading $MOD"
insmod "$ROOT/kernel/$MOD.ko"
udevadm settle
if [[ ! -c $DEV ]]; then
	echo "$DEV did not appear" >&2
	exit 1
fi
ls -l "$DEV"

echo "== running tests"
"$TEST" || RESULT=1

if command -v valgrind >/dev/null; then
	echo "== running tests under valgrind"
	valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all \
		--suppressions="$ROOT/scripts/valgrind.supp" \
		--error-exitcode=2 "$TEST" || RESULT=1
else
	echo "== valgrind not installed, skipping (apt install valgrind)"
fi

echo "== /proc/chardev_stats"
cat /proc/chardev_stats

cleanup
trap - EXIT

echo "== kernel log for this run"
dmesg | sed -n "/$MARKER/,\$p"

exit $RESULT
