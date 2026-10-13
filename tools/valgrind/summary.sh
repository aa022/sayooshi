#!/bin/sh
# summary.sh VG_OUT [TOOL] — summarises the logs of `make valgrind|helgrind|drd` (VG_OUT/<tool>/<run>.log, exit codes in
# <run>.rc): error count per run, then every error context attributed to the engine module of its first stack
# frame inside engine/src that is not allocator plumbing (core/mem*.c, core/str_wide.c), or "third-party" when no
# engine frame is on the stack; grouped by tool, module and kind. Exit 1 when any run failed.
# Decoders that hand their result to the caller (fmt/g00.c: g00_decode -> g00_free; fmt/pack.c: scene_str and
# scene_name fill the caller's WStr) count as plumbing too, so such a block is attributed to the module that owns
# it after the call, the one that must release it.
# Needs the logs written with --fullpath-after=engine/src/ (frames read "(gfx/canvas.c:123)").
set -eu
out=$1
fail=0
printf '%-9s %-10s %6s  %s\n' tool run errors "exit (99 = errors found)"
for d in "$out"/${2:-*}/; do
  tool=$(basename "$d")
  for l in "$d"*.log; do
    [ -f "$l" ] || continue
    run=$(basename "$l" .log)
    rc=$(cat "$d$run.rc" 2>/dev/null || echo '?')
    n=$(sed -n 's/.*ERROR SUMMARY: \([0-9][0-9]*\) errors.*/\1/p' "$l" | tail -n 1)
    printf '%-9s %-10s %6s  %s\n' "$tool" "$run" "${n:-?}" "$rc"
    [ "$rc" = 0 ] || fail=1
  done
done
echo
echo "error contexts by tool / module (first engine frame) / kind:"
for d in "$out"/${2:-*}/; do
  tool=$(basename "$d")
  for l in "$d"*.log; do
    [ -f "$l" ] || continue
    awk -v tool="$tool" '
      function flush() { if (kind != "") { cnt[tool " " (mod == "" ? "third-party" : mod) " | " kind]++ } kind = ""; mod = "" }
      { sub(/^==[0-9]+== ?/, "") }
      /^(---Thread-Announcement|-{10,})/ { flush(); skip = 1; next }
      /^$/ { flush(); skip = 0; next }
      skip { next }
      /^ +(at|by) 0x/ { if (kind != "" && mod == "") {
                           if (match($0, /\((app|core|fmt|gfx|plat|sav|snd|txt|ui|vm)\/[A-Za-z0-9_]+\.[ch]:[0-9]+\)/)) {
                             m = substr($0, RSTART + 1, RLENGTH - 2)
                             handoff = m ~ /^fmt\/g00\.c/ || $0 ~ /: scene_(str|name) \(fmt\/pack\.c/
                             if (m !~ /^core\/(mem[a-z_]*|str_wide)\.c/ && !handoff) { sub(/\/.*/, "", m); mod = m } }
                           else if ($0 ~ /\(sayopack\.c:[0-9]+\)/) mod = "tools" }
                       next }
      /^ / { next }
      /^(Thread [0-9]+:$|drd, |the cost of reduced accuracy|Memcheck|Helgrind|DRD|Copyright|Using|Command|Parent PID|HEAP SUMMARY|LEAK SUMMARY|ERROR SUMMARY|in use at exit|total heap usage|For lists|For counts|Rerun with|All heap blocks|To see them|Use --|Reachable blocks|definitely lost|indirectly lost|possibly lost|still reachable|suppressed|Thread #[0-9]+ was created|Thread #[0-9]+ is the program|Process terminating|Warning: |FATAL|Note: )/ { next }
      kind == "" { k = $0; gsub(/0x[0-9A-Fa-f]+/, "ADDR", k); gsub(/[0-9][0-9,]*/, "N", k); kind = k; next }
      END { flush(); for (c in cnt) printf "%5d  %s\n", cnt[c], c }' "$l"
  done
done | awk '{ k = substr($0, 8); c[k] += $1 } END { for (x in c) printf "%5d  %s\n", c[x], x }' | sort -k2,2 -k3,3 -k1,1rn
exit $fail
