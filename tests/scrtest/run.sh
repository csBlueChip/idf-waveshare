#!/bin/sh
# Build the interpreter core with the PC host and run every tests/scrtest/scripts/t_*.s
# (-test layout), then card/card_check.s in a temporary copy of sd/ (card layout).
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
	"$out" "$here/scripts" "$n" -test $opts > "$here/scripts/$n.got" 2>&1 || true
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

# Card layout: card/card_check.s runs in a temporary copy of the project's
# sd/ folder (real data files; the project's sd/ is never written).
tmp=$(mktemp -d)
cp -R "$here/../../sd/." "$tmp/"
cp "$here/card/card_check.s" "$tmp/scripts/"
{
	"$out" "$tmp" card_check -dump card.var || true
	echo "[files in scripts/data]"
	ls "$tmp/scripts/data" | sort
} > "$here/card/card_check.got" 2>&1
rm -rf "$tmp"
if [ "$1" = "-update" ]; then
	cp "$here/card/card_check.got" "$here/card/card_check.out"
fi
if cmp -s "$here/card/card_check.got" "$here/card/card_check.out"; then
	pass=$((pass + 1))
	rm -f "$here/card/card_check.got"
else
	fail=$((fail + 1))
	echo "FAIL card_check (diff card/card_check.out card/card_check.got)"
fi
echo "passed $pass, failed $fail"
[ "$fail" = 0 ]
