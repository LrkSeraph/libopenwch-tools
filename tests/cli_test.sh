#!/bin/sh
#
# Black-box tests for the wchlink command line.
#
# These check the contract a script would rely on: which exit code means what,
# and that a missing programmer is a diagnostic rather than a crash.  They run
# with no hardware attached, and also pass on a machine that happens to have a
# programmer plugged in.
#
# PROJECT must point at the built binary.

set -u

PROJECT="${PROJECT:-../wchlink}"

if [ ! -x "$PROJECT" ]; then
	echo "  FAIL  no executable at $PROJECT" >&2
	exit 1
fi

failures=0
checks=0

check_exit() {
	# check_exit <expected...> <description> ; reads $? from the caller
	status=$1
	shift
	description=$1
	shift

	checks=$((checks + 1))
	for want in "$@"; do
		if [ "$status" = "$want" ]; then
			return 0
		fi
	done

	failures=$((failures + 1))
	echo "  FAIL  $description: exit $status, wanted one of: $*"
}

# --- argument handling --------------------------------------------------

"$PROJECT" --version >/dev/null 2>&1
check_exit "$?" "--version succeeds" 0

"$PROJECT" --help >/dev/null 2>&1
check_exit "$?" "--help succeeds" 0

"$PROJECT" chips >/dev/null 2>&1
check_exit "$?" "chips succeeds" 0

"$PROJECT" >/dev/null 2>&1
check_exit "$?" "no arguments is a usage error" 2

"$PROJECT" nosuchcommand >/dev/null 2>&1
check_exit "$?" "unknown command is a usage error" 2

"$PROJECT" chips --nonsense >/dev/null 2>&1
check_exit "$?" "unknown option is a usage error" 2

"$PROJECT" info --chip nosuchpart >/dev/null 2>&1
check_exit "$?" "unknown part is a usage error" 2

"$PROJECT" programmer nosuchsub >/dev/null 2>&1
check_exit "$?" "unknown programmer subcommand is a usage error" 2

"$PROJECT" read >/dev/null 2>&1
check_exit "$?" "read without a file is a usage error" 2

# --- output content -----------------------------------------------------

# The table must contain the parts the tool is for.
out=$("$PROJECT" chips 2>/dev/null)
for part in ch32v003 ch582 ch583; do
	checks=$((checks + 1))
	case "$out" in
	*"$part"*) ;;
	*)
		failures=$((failures + 1))
		echo "  FAIL  chips does not mention $part"
		;;
	esac
done

# A known part with a full order code must be accepted, not rejected.
# info --chip now answers from the built-in table without opening USB.
"$PROJECT" info --chip ch32v003f4p6 >/dev/null 2>&1
check_exit "$?" "info accepts a full order code" 0

# --- missing hardware is a diagnostic, not a crash ----------------------

# Exit 3 is "no usable programmer".  0 means one was found and opened; both are
# success as far as this test is concerned.  A signal (128+) is a crash.
"$PROJECT" info >/dev/null 2>&1
status=$?
checks=$((checks + 1))
if [ "$status" -ge 128 ]; then
	failures=$((failures + 1))
	echo "  FAIL  info crashed with signal (exit $status)"
elif [ "$status" != 0 ] && [ "$status" != 1 ] && [ "$status" != 3 ]; then
	failures=$((failures + 1))
	echo "  FAIL  info exit $status, wanted 0 (detected), 1 (target error) or 3 (no programmer)"
fi

# The same, and it must say something useful on stderr.
err=$("$PROJECT" info 2>&1 >/dev/null)
checks=$((checks + 1))
if [ -n "$err" ] && [ "$status" = 3 ]; then
	case "$err" in
	*programmer*) ;;
	*)
		# Not a programmer problem: only complain if no programmer is present
		# at all, which is the case that produced exit 3.
		failures=$((failures + 1))
		echo "  FAIL  'no programmer' message does not mention a programmer"
		;;
	esac
fi

# --- flashing is refused before the bus is touched ----------------------

# The CH5xx families have no flash algorithm in this build, and saying so
# before opening a programmer means the message is the same with or without
# hardware.  1 is "target or tool error"; 2 would mean the parser accepted a
# command it should not, and 3 would mean it went looking for a programmer.
tmp=$(mktemp)
printf 'not a real image' >"$tmp"
big=$(mktemp)
head -c 32768 /dev/zero >"$big"

"$PROJECT" flash "$tmp" --chip ch582 >/dev/null 2>&1
check_exit "$?" "flash refuses a family with no algorithm" 1

# An image that does not fit the part is a usage error, and is refused before
# any hardware is touched -- so this answer does not depend on a programmer
# being attached.
"$PROJECT" flash "$big" --chip ch32v003 >/dev/null 2>&1
check_exit "$?" "flash refuses an image that does not fit" 2

# read defaults to the whole flash only when both range options are absent;
# one without the other is ambiguous and must be rejected before USB is touched.
"$PROJECT" read "$tmp" --address 0x08000000 >/dev/null 2>&1
check_exit "$?" "read with an address but no size is a usage error" 2

"$PROJECT" read "$tmp" --size 16 >/dev/null 2>&1
check_exit "$?" "read with a size but no address is a usage error" 2

rm -f "$tmp" "$big"

echo "  $checks checks, $failures failure(s)"

[ "$failures" -eq 0 ]
