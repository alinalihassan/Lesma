#!/bin/bash

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)

# Parallelism: default to CPU count, override with LESMA_TEST_JOBS (e.g. 1 for serial).
NUM_JOBS="${LESMA_TEST_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
case "${NUM_JOBS}" in
'' | *[!0-9]*) NUM_JOBS=4 ;;
esac
if [ "${NUM_JOBS}" -lt 1 ]; then
  NUM_JOBS=1
fi

fail_count=0
success_count=0

test_compiler() {
  local file="$1"
  local mode="$2"
  local compiler_path="$3"
  local quiet="${4:-}"
  if [ -n "${quiet}" ]; then
    "${compiler_path}" "${mode}" --no-warnings "${file}" >/dev/null 2>&1
  else
    "${compiler_path}" "${mode}" --no-warnings "${file}"
  fi
  return $?
}

# Writes one status line to result_file only (stdout stays free for compiler output).
# Lines: pass NAME EXPECT_FAIL | fail-run NAME EXPECT GOT | fail-expect NAME
run_single_test() {
  local file="$1"
  local expected_to_fail="$2"
  local compiler_path="$3"
  local result_file="$4"
  local name test_jit_ret_value test_expected_ret_value=0

  name=$(basename -s .les "${file}")

  if [ "${expected_to_fail}" -eq 1 ]; then
    test_compiler "${file}" "run" "${compiler_path}" quiet
    test_jit_ret_value=$?
  else
    test_compiler "${file}" "run" "${compiler_path}"
    test_jit_ret_value=$?
  fi

  if [ "${test_jit_ret_value}" -ne "${test_expected_ret_value}" ] && [ "${expected_to_fail}" -eq 0 ]; then
    printf 'fail-run %s %s %s\n' "${name}" "${test_expected_ret_value}" "${test_jit_ret_value}" >"${result_file}"
  elif [ "${expected_to_fail}" -eq 1 ] && [ "${test_jit_ret_value}" -eq "${test_expected_ret_value}" ]; then
    printf 'fail-expect %s\n' "${name}" >"${result_file}"
  else
    printf 'pass %s %s\n' "${name}" "${expected_to_fail}" >"${result_file}"
  fi
}

result_path_for_file() {
  local file="$1"
  local base
  base=$(printf '%s' "${file}" | tr '/' '_')
  printf '%s' "${base}.result"
}

process_result_line() {
  local line="$1"
  # shellcheck disable=SC2086
  set -- ${line}
  case "$1" in
  pass)
    success_count=$((success_count + 1))
    printf 'Testing %s\n' "$2"
    if [ "$3" -eq 1 ]; then
      printf '  %s\n' "Fail succeeded"
    else
      printf '  %s\n' "Run succeeded"
    fi
    ;;
  fail-run)
    fail_count=$((fail_count + 1))
    printf 'Testing %s\n' "$2"
    printf '  Run failed, expected %s, got %s\n' "$3" "$4"
    ;;
  fail-expect)
    fail_count=$((fail_count + 1))
    printf 'Testing %s\n' "$2"
    printf '  Run succeeded but was expected to fail\n'
    ;;
  *)
    printf 'Internal error: bad result line: %s\n' "${line}" >&2
    exit 1
    ;;
  esac
}

run_suite_parallel() {
  local tmpdir="$1"
  local expected_to_fail="$2"
  shift 2
  local -a files=("$@")
  local -a pids=()
  local file pid newp

  for file in "${files[@]}"; do
    while :; do
      newp=()
      for pid in "${pids[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
          newp+=("${pid}")
        fi
      done
      pids=("${newp[@]}")
      [ "${#pids[@]}" -lt "${NUM_JOBS}" ] && break
      sleep 0.05
    done
    (
      rp="${tmpdir}/$(result_path_for_file "${file}")"
      run_single_test "${file}" "${expected_to_fail}" "${compiler_path}" "${rp}"
    ) &
    pids+=($!)
  done

  for pid in "${pids[@]}"; do
    wait "${pid}" || true
  done
}

print_suite_results_in_order() {
  local tmpdir="$1"
  shift
  local -a files=("$@")
  local file rp line

  for file in "${files[@]}"; do
    rp="${tmpdir}/$(result_path_for_file "${file}")"
    if [ ! -f "${rp}" ]; then
      printf 'Internal error: missing result for %s\n' "${file}" >&2
      exit 1
    fi
    line=$(cat "${rp}")
    process_result_line "${line}"
  done
}

# Default: first arg, or auto-detect from common build layouts
if [ $# -gt 0 ]; then
  compiler_path="$1"
else
  ROOT="${SCRIPT_DIR}/.."
  if [ -x "${ROOT}/build/Debug/lesma" ]; then
    compiler_path="${ROOT}/build/Debug/lesma"
  elif [ -x "${ROOT}/build/lesma" ]; then
    compiler_path="${ROOT}/build/lesma"
  else
    printf 'Error: lesma binary not found. Build the project first, or pass the path:\n  %s <path-to-lesma>\n' "$(basename "$0")" >&2
    exit 1
  fi
fi

TMPDIR_RESULTS=$(mktemp -d)
trap 'rm -rf "${TMPDIR_RESULTS}"' EXIT

# shellcheck disable=SC2086
success_files=("${SCRIPT_DIR}"/../tests/lesma/success/*.les)
failure_files=("${SCRIPT_DIR}"/../tests/lesma/failure/*.les)

run_suite_parallel "${TMPDIR_RESULTS}" 0 "${success_files[@]}"
print_suite_results_in_order "${TMPDIR_RESULTS}" "${success_files[@]}"

run_suite_parallel "${TMPDIR_RESULTS}" 1 "${failure_files[@]}"
print_suite_results_in_order "${TMPDIR_RESULTS}" "${failure_files[@]}"

printf 'Tests:\n'
printf '  fail:    %d\n' "${fail_count}"
printf '  success: %d\n' "${success_count}"
if [ "${fail_count}" -gt 0 ]; then
  exit 1
fi
exit 0
