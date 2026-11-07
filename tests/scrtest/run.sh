#!/bin/sh
# Build the interpreter core with the PC host and run every tests/scrtest/scripts/t_*.s.
# Each t_name.s is compared with t_name.out (the expected output).
# ./run.sh            run all, report PASS / FAIL
# ./run.sh -update    rewrite the .out files from the current output (after checking!)
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../src"
out=${TMPDIR:-/tmp}/scrtest
gcc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O1 -I"$src" \
	"$here/host.c" "$src/scr_load.c" "$src/scr_eval.c" "$src/scr_exec.c" -lm -o "$out"
cd "$here"			# paths in .opt files are relative to tests/scrtest
pass=0
fail=0
for f in "$here"/scripts/t_*.s; do
	n=$(basename "$f" .s)
	opts=""
	[ -f "$here/scripts/$n.opt" ] && opts=$(cat "$here/scripts/$n.opt")
	# shellcheck disable=SC2086
	"$out" "$here/scripts" "$n" $opts > "$here/scripts/$n.got" 2>&1 || true
	if [ "$1" = "-update" ]; then
		cp "$here/scripts/$n.got" "$here/scripts/$n.out"
	fi
	if cmp -s "$here/scripts/$n.got" "$here/scripts/$n.out"; then
		pass=$((pass + 1))
		rm -f "$here/scripts/$n.got"
	else
		fail=$((fail + 1))
		echo "FAIL $n (diff $n.out $n.got)"
	fi
done
echo "passed $pass, failed $fail"
[ "$fail" = 0 ]
