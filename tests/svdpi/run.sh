#!/usr/bin/env bash
# Real-simulator run of the SVDPI backend (Synopsys VCS), categories 2-4 of
# the four-category suite (category 1 is the SystemC unit suite, `xmake test`):
#
#   category 2  tests/svdpi/cpp   C++ side of simlog, running the shared scenario
#                                 bodies from tests/cases.cpp — the same ones the
#                                 SystemC suite runs (category 1)
#   category 3  tests/svdpi/sv    SV side through the sv/simlog_pkg.sv proxy class
#   category 4  tests/svdpi/mixed C++ and SV through one logger entity (one
#                                 shared object, so one registry)
#
# Usage (the xmake target passes the three libraries):
#
#   xmake run vcs-test
#   tests/svdpi/run.sh <libsimlog_svdpi_cpp.so> <libsimlog.so> <libsimlog_svdpi_mixed.so>
#
# Category 2 asserts its log lines inside C++ (the shared scenarios), so the
# bench's exit status is the result. Categories 3 and 4 cannot capture stdout
# from SystemVerilog, so their exact lines are expected on stdin here and diffed.
#
# Skipped (exit 0) when `vcs` is not on PATH, so the target is safe to run
# everywhere.
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
cpp_lib=${1:-}
bridge=${2:-}
mixed_lib=${3:-}

if ! command -v vcs >/dev/null 2>&1; then
  echo "[svdpi-test] skipped: no vcs on PATH (the SVDPI backend is only verified in a simulator)"
  exit 0
fi
if [[ -z $cpp_lib || ! -f $cpp_lib || -z $bridge || ! -f $bridge || -z $mixed_lib ||
  ! -f $mixed_lib ]]; then
  echo "[svdpi-test] usage: run.sh <cpp.so> <bridge.so> <mixed.so>" >&2
  exit 2
fi
cpp_lib=$(cd -- "$(dirname -- "$cpp_lib")" && pwd)/$(basename -- "$cpp_lib")
bridge=$(cd -- "$(dirname -- "$bridge")" && pwd)/$(basename -- "$bridge")
mixed_lib=$(cd -- "$(dirname -- "$mixed_lib")" && pwd)/$(basename -- "$mixed_lib")

work="$root/build/tests-svdpi"
failures=0

# compile_simv <dir> <timescale> <vcs file list...>: compile once per
# (directory, timescale) and keep the work tree for the logs.
compile_simv() {
  local dir=$1 timescale=$2
  shift 2
  mkdir -p "$dir"
  if ! (cd "$dir" &&
    timeout 900 vcs -full64 -sverilog -q -override_timescale="$timescale" \
      +incdir+"$root/sv" "$@" -o simv >compile.log 2>&1 </dev/null); then
    echo "[svdpi-test] FAIL: vcs failed in $dir, see $dir/compile.log" >&2
    tail -n 8 "$dir/compile.log" >&2
    return 1
  fi
}

# run_simv <dir> <logfile> <SIMLOG_LEVEL or ""> [plusargs...]
# The level is set or unset inside the subshell, so a SIMLOG_LEVEL inherited from
# the caller cannot make a non-env case non-hermetic (the SV-only categories have
# no C++ prepare() to unset it for them).
run_simv() {
  local dir=$1 logfile=$2 level=$3
  shift 3
  (
    cd "$dir" || exit 1
    if [[ -n $level ]]; then
      SIMLOG_LEVEL="$level" timeout 300 ./simv "$@" >"$logfile" 2>&1 </dev/null
    else
      unset SIMLOG_LEVEL
      timeout 300 ./simv "$@" >"$logfile" 2>&1 </dev/null
    fi
  )
}

# ── Category 2: C++ side, shared scenarios ───────────────────────────
# The whole shared suite runs at the canonical 1 ps base; format and stamp also
# run at every other supported timescale, so one scenario covers both the API
# contract and the backend's tick/precision reading.
CPP_SCENARIOS=(format stamp env-default env-warn env-off env-invalid env-modules
  env-modules-wildcard env-modules-global env-modules-order env-modules-invalid
  registry parse-level)

run_cpp_group() {
  local tag=$1 timescale=$2 scale=$3 unit=$4
  shift 4
  local dir="$work/cpp-$tag"
  local scn logfile

  echo "[svdpi-test] category 2 (cpp): building $tag, timescale $timescale"
  if ! compile_simv "$dir" "$timescale" "$here/cpp/tb_cpp.sv" "$cpp_lib" "$bridge"; then
    echo "[svdpi-test] FAIL category-2/$tag: vcs failed" >&2
    failures=$((failures + 1))
    return
  fi

  for scn in "$@"; do
    logfile="$dir/$scn.log"
    if ! run_simv "$dir" "$logfile" "" "+scenario=$scn" "+scale=$scale" "+unit=$unit"; then
      echo "[svdpi-test] FAIL category-2/$scn ($tag): simv exited non-zero, see $logfile" >&2
      tail -n 8 "$logfile" >&2
      failures=$((failures + 1))
      continue
    fi
    echo "[svdpi-test] ok category-2/$scn ($tag)"
  done
}

run_cpp_group 1ps 1ps/1ps 1 ps "${CPP_SCENARIOS[@]}"
run_cpp_group 100fs 1ns/100fs 100 fs format stamp
run_cpp_group 1ns 1ns/1ns 1 ns format stamp
run_cpp_group 1fs 1ns/1fs 1 fs format stamp

# ── Category 3: SV proxy class ───────────────────────────────────────
SV_DIR="$work/sv"
echo "[svdpi-test] category 3 (sv): building the proxy-class bench"
if compile_simv "$SV_DIR" "1ps/1ps" "$root/sv/simlog_pkg.sv" "$here/sv/proxy.sv" "$bridge"; then
  # check_sv <name> <SIMLOG_LEVEL or ""> <required stderr text or "">, expected
  # log lines on stdin. Only log lines are diffed; the [simlog] notices are
  # checked separately so an invalid-argument case can assert on them.
  check_sv() {
    local name=$1 level=$2 stderr_text=$3
    local dir="$work/sv-runs/$name"

    mkdir -p "$dir"
    cat >"$dir/expected.txt"
    if ! run_simv "$SV_DIR" "$dir/run.log" "$level" "+scenario=$name"; then
      echo "[svdpi-test] FAIL category-3/$name: simv exited non-zero, see $dir/run.log" >&2
      tail -n 8 "$dir/run.log" >&2
      failures=$((failures + 1))
      return
    fi
    grep -E '^\[ +[0-9]' "$dir/run.log" >"$dir/actual.txt"
    if ! diff -u "$dir/expected.txt" "$dir/actual.txt"; then
      echo "[svdpi-test] FAIL category-3/$name: log lines differ (expected above, actual below)" >&2
      failures=$((failures + 1))
      return
    fi
    if [[ -n $stderr_text ]] && ! grep -q -- "$stderr_text" "$dir/run.log"; then
      echo "[svdpi-test] FAIL category-3/$name: missing stderr notice: $stderr_text" >&2
      failures=$((failures + 1))
      return
    fi
    echo "[svdpi-test] ok category-3/$name"
  }

  check_sv levels "" "" <<'EOF'
[           0 ps][tb][info] info kept
[           0 ps][tb][warning] warn kept
[           0 ps][tb][error] error kept
[           0 ps][tb][critical] critical kept
[          10 ps][tb][info] info after delay
EOF

  check_sv format "" "" <<'EOF'
[           0 ps][tb][info] plain
[           0 ps][tb][warning] braces {} percent % value=42
[           0 ps][tb][error] format chars: %d %s {
[          20 ps][tb][info] after 20
EOF

  check_sv modules "" "" <<'EOF'
[           0 ps][tb][info] tb first
[           0 ps][dma0][info] dma first
[          10 ps][dma0][warning] dma second
[          10 ps][tb][info] tb second
EOF

  check_sv env "tb=warn" "" <<'EOF'
[           0 ps][tb][warning] tb warn kept
[           0 ps][dma0][info] dma0 info kept
EOF

  check_sv empty-name "" "simlog_open: empty logger name" <<'EOF'
[           0 ps][tb][info] valid
EOF
else
  echo "[svdpi-test] FAIL category-3: vcs failed" >&2
  failures=$((failures + 1))
fi

# Category 4 — mixed C++ + SV, one logger entity
# ── Category 4: mixed C++ + SV, one logger entity ────────────────────
MIXED_DIR="$work/mixed"
echo "[svdpi-test] category 4 (mixed): building the mixed bench"
# Only the mixed library is loaded: it links libsimlog.so (via $ORIGIN), so the
# bridge and simlog::get() share one registry.
if compile_simv "$MIXED_DIR" "1ps/1ps" "$root/sv/simlog_pkg.sv" "$here/mixed/tb_mixed.sv" \
  "$mixed_lib"; then
  check_mixed() {
    local name=$1 level=$2
    local dir="$work/mixed-runs/$name"

    mkdir -p "$dir"
    cat >"$dir/expected.txt"
    if ! run_simv "$MIXED_DIR" "$dir/run.log" "$level" "+scenario=$name"; then
      echo "[svdpi-test] FAIL category-4/$name: simv exited non-zero, see $dir/run.log" >&2
      tail -n 8 "$dir/run.log" >&2
      failures=$((failures + 1))
      return
    fi
    grep -E '^\[ +[0-9]' "$dir/run.log" >"$dir/actual.txt"
    if ! diff -u "$dir/expected.txt" "$dir/actual.txt"; then
      echo "[svdpi-test] FAIL category-4/$name: log lines differ (expected above, actual below)" >&2
      failures=$((failures + 1))
      return
    fi
    echo "[svdpi-test] ok category-4/$name"
  }

  check_mixed order "" <<'EOF'
[           0 ps][tb][info] cpp first
[           0 ps][tb][info] sv second
[          10 ps][tb][warning] sv third
[          10 ps][tb][info] cpp fourth
EOF

  check_mixed sv-first "" <<'EOF'
[           0 ps][tb][info] sv first
[           0 ps][tb][info] cpp second
[          10 ps][tb][error] cpp third
[          10 ps][tb][warning] sv fourth
EOF

  check_mixed level "tb=warn" <<'EOF'
[           0 ps][tb][warning] sv warn kept
[           0 ps][tb][warning] cpp warn kept
EOF
else
  echo "[svdpi-test] FAIL category-4: vcs failed" >&2
  failures=$((failures + 1))
fi

if [[ $failures -ne 0 ]]; then
  echo "[svdpi-test] $failures check(s)/case(s) failed" >&2
  exit 1
fi
echo "[svdpi-test] all categories passed"
