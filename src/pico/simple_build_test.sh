#!/usr/bin/env bash

# Simple build benchmark focused on a complex Pico W build.
# Compares serial and selected parallel build levels to help narrow down
# performance issues in the heavy cyw43/lwIP/mbedtls/pioasm build path.
#
# Usage:
#   ./src/pico/simple_build_test.sh
#   BUILD_DIR=build/pico-test-parallel ./src/pico/simple_build_test.sh
#   PARALLEL_LEVELS="4 6 8 12" ./simple_build_test.sh
#   RUNS_PER_LEVEL=2 ./simple_build_test.sh
#
# Notes:
# - This script intentionally uses a single complex board/configuration.
# - Each run starts from a clean build directory.
# - Configure and build phases are timed separately.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-build/pico-test-parallel}"
RUNS_PER_LEVEL="${RUNS_PER_LEVEL:-1}"
PARALLEL_LEVELS_STRING="${PARALLEL_LEVELS:-4 6 8 12}"

# Use a complex board/config that exercises the heavy Pico W build path.
PICO_BOARD_VALUE="${PICO_BOARD_VALUE:-pico2_w}"
CMAKE_OPTS=(
  -DCMAKE_BUILD_TYPE=Release
  -DPICO_BOARD="${PICO_BOARD_VALUE}"
  -DSKIP_VERSION_INCREMENT=1
  -DALTAIR_DEBUG=OFF
  -DREMOTE_FS_SUPPORT=ON
  -DRFS_SERVER_PORT=8085
)

now_ms() {
  python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
}

format_ms() {
  local ms="$1"
  python3 - "$ms" <<'PY'
import sys
ms = int(sys.argv[1])
print(f"{ms/1000:.3f}s")
PY
}

format_ratio() {
  local base_ms="$1"
  local test_ms="$2"
  python3 - "$base_ms" "$test_ms" <<'PY'
import sys
base_ms = int(sys.argv[1])
test_ms = int(sys.argv[2])
if base_ms <= 0:
    print("n/a")
else:
    print(f"{test_ms / base_ms:.2f}x")
PY
}

printf_avg_ms() {
  python3 - "$@" <<'PY'
import sys
values = [int(v) for v in sys.argv[1:] if v.strip()]
if not values:
    print(0)
else:
    print(sum(values) // len(values))
PY
}

run_case() {
  local jobs="$1"
  local run_number="$2"
  local configure_ms build_ms total_ms
  local configure_start_ms configure_end_ms build_start_ms build_end_ms total_start_ms total_end_ms
  local log_file="${REPO_ROOT}/build/pico-simple-build-logs/build_j${jobs}_run${run_number}.log"

  echo -e "${YELLOW}Run ${run_number}, parallel=${jobs}: cleaning ${BUILD_DIR}${NC}" >&2
  rm -rf "$BUILD_DIR"

  total_start_ms="$(now_ms)"

  configure_start_ms="$(now_ms)"
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja "${CMAKE_OPTS[@]}" 2>&1 | tee "$log_file" >&2
  configure_end_ms="$(now_ms)"
  configure_ms=$((configure_end_ms - configure_start_ms))

  build_start_ms="$(now_ms)"
  cmake --build "$BUILD_DIR" --parallel "$jobs" 2>&1 | tee -a "$log_file" >&2
  build_end_ms="$(now_ms)"
  build_ms=$((build_end_ms - build_start_ms))

  total_end_ms="$(now_ms)"
  total_ms=$((total_end_ms - total_start_ms))

  echo "${configure_ms}|${build_ms}|${total_ms}|${log_file}"
}

print_banner() {
  printf "%b\n" "$1"
}

mkdir -p "${REPO_ROOT}/build/pico-simple-build-logs"

IFS=' ' read -r -a PARALLEL_LEVELS <<< "$PARALLEL_LEVELS_STRING"

if [ "${#PARALLEL_LEVELS[@]}" -eq 0 ]; then
  echo -e "${RED}No PARALLEL_LEVELS provided.${NC}"
  exit 1
fi

if ! [[ "$RUNS_PER_LEVEL" =~ ^[0-9]+$ ]] || [ "$RUNS_PER_LEVEL" -lt 1 ]; then
  echo -e "${RED}RUNS_PER_LEVEL must be a positive integer.${NC}"
  exit 1
fi

print_banner "${BLUE}=================================${NC}"
print_banner "${BLUE}Complex Build Parallelism Benchmark${NC}"
print_banner "${BLUE}=================================${NC}"
print_banner "${BLUE}Board: ${PICO_BOARD_VALUE}${NC}"
print_banner "${BLUE}Build dir: ${BUILD_DIR}${NC}"
print_banner "${BLUE}Parallel levels: ${PARALLEL_LEVELS_STRING}${NC}"
print_banner "${BLUE}Runs per level: ${RUNS_PER_LEVEL}${NC}"
echo ""

RESULT_LINES=()
BASELINE_TOTAL_MS=""
BASELINE_JOBS=""

for jobs in "${PARALLEL_LEVELS[@]}"; do
  if ! [[ "$jobs" =~ ^[0-9]+$ ]] || [ "$jobs" -lt 1 ]; then
    echo -e "${RED}Invalid parallel level: ${jobs}${NC}"
    exit 1
  fi

  print_banner "${BLUE}Testing --parallel ${jobs}${NC}"

  configure_samples=()
  build_samples=()
  total_samples=()
  last_log_file=""

  run_idx=1
  while [ "$run_idx" -le "$RUNS_PER_LEVEL" ]; do
    result="$(run_case "$jobs" "$run_idx")"
    IFS='|' read -r configure_ms build_ms total_ms log_file <<< "$result"

    configure_samples+=("$configure_ms")
    build_samples+=("$build_ms")
    total_samples+=("$total_ms")
    last_log_file="$log_file"

    print_banner "  ${GREEN}run ${run_idx}:${NC} configure $(format_ms "$configure_ms"), build $(format_ms "$build_ms"), total $(format_ms "$total_ms")"
    run_idx=$((run_idx + 1))
  done

  avg_configure_ms="$(printf_avg_ms "${configure_samples[@]}")"
  avg_build_ms="$(printf_avg_ms "${build_samples[@]}")"
  avg_total_ms="$(printf_avg_ms "${total_samples[@]}")"

  if [ -z "$BASELINE_TOTAL_MS" ]; then
    BASELINE_TOTAL_MS="$avg_total_ms"
    BASELINE_JOBS="$jobs"
  fi

  ratio_to_baseline="$(format_ratio "$BASELINE_TOTAL_MS" "$avg_total_ms")"
  RESULT_LINES+=("${jobs}|${avg_configure_ms}|${avg_build_ms}|${avg_total_ms}|${ratio_to_baseline}|${last_log_file}")

  print_banner "  ${YELLOW}average:${NC} configure $(format_ms "$avg_configure_ms"), build $(format_ms "$avg_build_ms"), total $(format_ms "$avg_total_ms")"
  echo ""
done

REPORT_FILE="${REPO_ROOT}/build/pico-simple-build-logs/parallel_benchmark_report.txt"

{
  echo "================================="
  echo "Complex Build Parallelism Benchmark"
  echo "================================="
  echo "Date: $(date)"
  echo "Board: ${PICO_BOARD_VALUE}"
  echo "Build dir: ${BUILD_DIR}"
  echo "Parallel levels: ${PARALLEL_LEVELS_STRING}"
  echo "Runs per level: ${RUNS_PER_LEVEL}"
  echo "Baseline jobs: ${BASELINE_JOBS}"
  echo ""
  printf "%-10s %-14s %-14s %-14s %-12s %s\n" "Parallel" "Avg Configure" "Avg Build" "Avg Total" "Vs Baseline" "Log"
  printf "%-10s %-14s %-14s %-14s %-12s %s\n" "----------" "--------------" "--------------" "--------------" "------------" "---"
  for line in "${RESULT_LINES[@]}"; do
    IFS='|' read -r jobs avg_configure_ms avg_build_ms avg_total_ms ratio_to_baseline log_file <<< "$line"
    printf "%-10s %-14s %-14s %-14s %-12s %s\n" \
      "$jobs" \
      "$(format_ms "$avg_configure_ms")" \
      "$(format_ms "$avg_build_ms")" \
      "$(format_ms "$avg_total_ms")" \
      "$ratio_to_baseline" \
      "$log_file"
  done
} > "$REPORT_FILE"

print_banner "${BLUE}=================================${NC}"
print_banner "${BLUE}Summary${NC}"
print_banner "${BLUE}=================================${NC}"
printf "%-10s %-14s %-14s %-14s %-12s\n" "Parallel" "Avg Configure" "Avg Build" "Avg Total" "Vs Baseline"
printf "%-10s %-14s %-14s %-14s %-12s\n" "----------" "--------------" "--------------" "--------------" "------------"
for line in "${RESULT_LINES[@]}"; do
  IFS='|' read -r jobs avg_configure_ms avg_build_ms avg_total_ms ratio_to_baseline log_file <<< "$line"
  printf "%-10s %-14s %-14s %-14s %-12s\n" \
    "$jobs" \
    "$(format_ms "$avg_configure_ms")" \
    "$(format_ms "$avg_build_ms")" \
    "$(format_ms "$avg_total_ms")" \
    "$ratio_to_baseline"
done

echo ""
echo -e "Report saved to: ${REPORT_FILE}"
echo -e "Logs saved to: ${REPO_ROOT}/build/pico-simple-build-logs"