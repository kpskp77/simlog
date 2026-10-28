#!/usr/bin/env bash
# Run the SVDPI example (default backend): examples/svdpi/tb.sv, a pure
# SystemVerilog testbench, against sv/simlog_pkg.sv and the DPI-C library
# libsimlog.so that `xmake` builds from src/simlog.cpp. There is no C++ in the
# example: the testbench only imports the proxy package.
#
#   xmake run example                            # through the xmake target
#   examples/svdpi/run.sh <build>/libsimlog.so   # by hand
#
# This is the local smoke test for the SVDPI backend, so it compares the exact
# log lines with the ones documented in README.md: a stamp or pattern change
# fails the run instead of silently making the docs stale. Skipped (exit 0) when
# `vcs` is not on PATH — the backend only runs inside a simulator, and
# tests/svdpi/run.sh skips itself the same way.
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
lib=${1:-}

if ! command -v vcs >/dev/null 2>&1; then
  echo "[example] skipped: no vcs on PATH (the SVDPI example runs in a simulator)"
  exit 0
fi
if [[ -z $lib || ! -f $lib ]]; then
  echo "[example] usage: run.sh <path/to/libsimlog.so>" >&2
  exit 2
fi
lib=$(cd -- "$(dirname -- "$lib")" && pwd)/$(basename -- "$lib")

work="$root/build/examples-svdpi"
mkdir -p "$work"

# -override_timescale, not just the bench's `timescale: sv/simlog_pkg.sv is
# compiled first and carries none, which VCS rejects next to a module that does.
# It also pins the precision the documented stamps assume (1 ps), the same way
# tests/svdpi/run.sh pins the time base it compiles for.
echo "[example] compiling examples/svdpi/tb.sv with $lib"
if ! (cd "$work" &&
  timeout 900 vcs -full64 -sverilog -q -override_timescale=1ps/1ps \
    "$root/sv/simlog_pkg.sv" "$here/tb.sv" "$lib" -o simv >compile.log 2>&1 </dev/null); then
  echo "[example] FAIL: vcs failed, see $work/compile.log" >&2
  tail -n 8 "$work/compile.log" >&2
  exit 1
fi

# SIMLOG_LEVEL is unset so an inherited value cannot change the documented
# output; the example's comments show how to use it.
if ! (cd "$work" && unset SIMLOG_LEVEL && timeout 300 ./simv >run.log 2>&1 </dev/null); then
  echo "[example] FAIL: simv exited non-zero, see $work/run.log" >&2
  tail -n 8 "$work/run.log" >&2
  exit 1
fi

# Filter the simulator's own messages: only the log lines are compared.
grep -E '^\[ +[0-9]' "$work/run.log" >"$work/actual.txt"
cat "$work/actual.txt"

cat >"$work/expected.txt" <<'EOF'
[           0 ps][tb][info] sim start
[       10000 ps][tb][info] cnt=1
[       15000 ps][tb.u_dut][warning] unexpected burst
[       20000 ps][tb][info] cnt=2
[       30000 ps][tb][info] cnt=3
[       30000 ps][tb][error] sim end
EOF

if ! diff -u "$work/expected.txt" "$work/actual.txt"; then
  echo "[example] FAIL: log lines differ (expected above, actual below)" >&2
  exit 1
fi
echo "[example] ok: the 6 documented lines"
