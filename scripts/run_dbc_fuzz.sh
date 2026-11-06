#!/usr/bin/env bash
# NFR-05: run both fuzz targets to a documented budget.
#
# Not a ctest case on purpose (research R-21): libFuzzer brings its own main
# and needs clang, so making it part of the suite would break the default GCC
# and Windows builds. This script is the reproducible evidence instead.
#
# Usage: scripts/run_dbc_fuzz.sh [runs-per-target] [build-dir]

set -euo pipefail
cd "$(dirname "$0")/.."

RUNS="${1:-1000000}"
BUILD_DIR="${2:-build-fuzz-wsl}"

if ! command -v clang++ >/dev/null 2>&1; then
  echo "clang++ is required for libFuzzer. Install it, or run the sanitizer"
  echo "suite instead: see quickstart.md section 1."
  exit 2
fi

echo "== configuring ($BUILD_DIR, clang, fuzzer+asan+ubsan) =="
cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DTCU_BUILD_FUZZERS=ON -DTCU_BUILD_TESTS=OFF -DTCU_BUILD_BENCHMARKS=OFF >/dev/null
cmake --build "$BUILD_DIR" --target fuzz_dbc_parser fuzz_frame_decoder -j"$(nproc)"

# libFuzzer MERGES the inputs it finds interesting back into the corpus
# directory it is given, so it is given a scratch directory that is thrown away.
# Two reasons, and neither is about speed: a fuzz run must not leave hundreds of
# untracked files in the repository, and a run must start from the same seeds
# every time. Accumulated leftovers made one earlier failure impossible to
# attribute, which is its own argument.
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tcu-dbc-fuzz-XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT
mkdir -p "$WORK_DIR/parser" "$WORK_DIR/frames"

# Seed from the committed fixtures: a fuzzer that starts from valid input
# reaches the interesting paths far sooner than one starting from noise.
cp -f configs/virtual_tcu.dbc "$WORK_DIR/parser/" 2>/dev/null || true
cp -f tests/fixtures/dbc/invalid/*.dbc "$WORK_DIR/parser/" 2>/dev/null || true
printf '\x01\x00\x00\x08\x10\x27\x01\x00\x50\x00\x00\x00' > "$WORK_DIR/frames/seed_vehicle"
printf '\x01\x01\x00\x08\xAB\xC0\x00\x00\x00\x00\x00\x00' > "$WORK_DIR/frames/seed_torque"
printf '\x01\x02\x00\x08\x01\x7F\xD2\x04\x00\x00\x00\x00' > "$WORK_DIR/frames/seed_mux"

# LeakSanitizer is disabled here, and that is a deliberate, narrow decision.
#
# On WSL2 LSan's at-exit scan segfaults inside libFuzzer's exit path - the
# target runs the input correctly (verified under gdb: "Executed ... in 6 ms")
# and then dies in the leak check, which looks exactly like a crash found by
# fuzzing and is not one. LSan needs process introspection the WSL2 kernel does
# not fully provide.
#
# Leaks are still covered: the ASan build of the test suites runs with leak
# detection ON and is clean. What is given up here is leak detection on
# fuzzer-generated inputs specifically, and that limitation belongs in the
# evidence rather than hidden in an environment variable.
export ASAN_OPTIONS="detect_leaks=0:${ASAN_OPTIONS:-}"
export UBSAN_OPTIONS="print_stacktrace=1:${UBSAN_OPTIONS:-}"

STATUS=0
for target in fuzz_dbc_parser fuzz_frame_decoder; do
  case "$target" in
    fuzz_dbc_parser) corpus="$WORK_DIR/parser" ;;
    *) corpus="$WORK_DIR/frames" ;;
  esac
  echo
  echo "== $target: $RUNS runs, corpus $corpus =="
  if ! "$BUILD_DIR/fuzz/dbc/$target" "$corpus" \
      -runs="$RUNS" -max_total_time="${MAX_TOTAL_TIME:-900}" \
      -print_final_stats=1 -rss_limit_mb=2048; then
    echo "FUZZING FAILED for $target - the crashing input is in the file named above."
    STATUS=1
  fi
done

echo
if [ "$STATUS" = "0" ]; then
  echo "Both targets completed with no sanitizer finding."
  echo "Record the build, the corpus and the run count alongside this result:"
  echo "  clang: $(clang++ --version | head -1)"
  echo "  runs:  $RUNS per target (or the $MAX_TOTAL_TIME s cap, whichever came first)"
  echo "  note:  leak detection was OFF for these runs - see the comment above"
  echo "  corpus: seeded from the committed fixtures into a scratch dir, discarded after"
else
  echo "At least one target failed. NFR-05 is NOT satisfied until this is clean."
fi
exit "$STATUS"
