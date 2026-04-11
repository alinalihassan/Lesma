#!/bin/bash

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." &>/dev/null && pwd)

# Print a filesystem path relative to REPO_ROOT (falls back to the original path if outside).
repo_relative_path() {
  local p="$1"
  local dir base ap
  dir=$(dirname -- "$p")
  base=$(basename -- "$p")
  ap=$(cd "$dir" 2>/dev/null && printf '%s/%s' "$(pwd)" "$base" || printf '%s' "$p")
  case "$ap" in
  "${REPO_ROOT}/"*) printf '%s\n' "${ap#"${REPO_ROOT}/"}" ;;
  "${REPO_ROOT}") printf '%s\n' "." ;;
  *) printf '%s\n' "$p" ;;
  esac
}

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
failed_tests=()

# Wall-clock limit per compiler invocation (seconds). Use 0 to disable. Override with LESMA_TEST_TIMEOUT.
LESMA_TEST_TIMEOUT="${LESMA_TEST_TIMEOUT:-2}"
# Success tests hide JIT program stdout by default. Set LESMA_TEST_VERBOSE=1 to print it.
LESMA_TEST_VERBOSE="${LESMA_TEST_VERBOSE:-0}"
case "${LESMA_TEST_TIMEOUT}" in
'' | *[!0-9]*) LESMA_TEST_TIMEOUT=2 ;;
esac
case "${LESMA_TEST_VERBOSE}" in
'' | *[!0-9]*) LESMA_TEST_VERBOSE=0 ;;
esac
LESMA_TEST_TIMEOUT=$((10#${LESMA_TEST_TIMEOUT}))
LESMA_TEST_VERBOSE=$((10#${LESMA_TEST_VERBOSE}))

# Watchdog timeout must not reuse a process exit code. The old LESMA_TEST_TIMEOUT_EXIT=124 matched
# GNU timeout but collided with forwarded codes: Driver returns LesmaError::getExitCode() (uint8_t,
# LesmaError.h) from Driver.cpp; src/cli/main.cpp forwards that to std::_Exit. Any 0–255 can be a
# real JIT/compiler exit. Timeouts are indicated only by LESMA_WATCHDOG_TIMED_OUT=1 (see test_compiler).

test_compiler() {
  local file="$1"
  local mode="$2"
  local compiler_path="$3"
  local quiet="${4:-}"
  local cpid kpid hard_pid ret timeout_flag

  LESMA_WATCHDOG_TIMED_OUT=0

  if [ "${LESMA_TEST_TIMEOUT}" -eq 0 ]; then
    case "${quiet}" in
    all)
      "${compiler_path}" "${mode}" --no-warnings "${file}" >/dev/null 2>&1
      ;;
    out)
      "${compiler_path}" "${mode}" --no-warnings "${file}" >/dev/null
      ;;
    *)
      "${compiler_path}" "${mode}" --no-warnings "${file}"
      ;;
    esac
    return $?
  fi

  timeout_flag=$(mktemp "${TMPDIR:-/tmp}/lesma-test-timeout.XXXXXX") || return 1

  case "${quiet}" in
  all)
    "${compiler_path}" "${mode}" --no-warnings "${file}" >/dev/null 2>&1 &
    ;;
  out)
    "${compiler_path}" "${mode}" --no-warnings "${file}" >/dev/null &
    ;;
  *)
    "${compiler_path}" "${mode}" --no-warnings "${file}" &
    ;;
  esac
  cpid=$!

  # Watchdog: after LESMA_TEST_TIMEOUT, SIGTERM then SIGKILL after ~1s if still alive.
  (
    sleep "${LESMA_TEST_TIMEOUT}"
    if kill -0 "${cpid}" 2>/dev/null; then
      printf '1' >"${timeout_flag}"
      kill -TERM "${cpid}" 2>/dev/null || true
      i=0
      while [ "${i}" -lt 10 ] && kill -0 "${cpid}" 2>/dev/null; do
        sleep 0.1
        i=$((i + 1))
      done
      if kill -0 "${cpid}" 2>/dev/null; then
        kill -KILL "${cpid}" 2>/dev/null || true
      fi
    fi
  ) &
  kpid=$!

  # Hard ceiling so wait on cpid cannot block without bound if signal delivery misbehaves.
  (
    sleep $((LESMA_TEST_TIMEOUT + 5))
    if kill -0 "${cpid}" 2>/dev/null; then
      kill -KILL "${cpid}" 2>/dev/null || true
      printf '1' >"${timeout_flag}"
    fi
  ) &
  hard_pid=$!

  wait "${cpid}"
  ret=$?

  kill "${kpid}" 2>/dev/null || true
  wait "${kpid}" 2>/dev/null || true
  kill "${hard_pid}" 2>/dev/null || true
  wait "${hard_pid}" 2>/dev/null || true

  if [ -s "${timeout_flag}" ]; then
    LESMA_WATCHDOG_TIMED_OUT=1
  fi
  rm -f "${timeout_flag}"

  return "${ret}"
}

# Writes one status line to result_file only.
# quiet modes: "out" = drop subprocess stdout (JIT program output); "all" = drop stdout+stderr.
# Lines: pass NAME EXPECT_FAIL | fail-run NAME EXPECT GOT | fail-expect NAME
run_single_test() {
  local file="$1"
  local expected_to_fail="$2"
  local compiler_path="$3"
  local result_file="$4"
  local name test_jit_ret_value test_expected_ret_value=0

  name=$(basename -s .les "${file}")

  if [ "${expected_to_fail}" -eq 1 ]; then
    test_compiler "${file}" "run" "${compiler_path}" all
    test_jit_ret_value=$?
  elif [ "${LESMA_TEST_VERBOSE}" -eq 1 ]; then
    test_compiler "${file}" "run" "${compiler_path}"
    test_jit_ret_value=$?
  else
    test_compiler "${file}" "run" "${compiler_path}" out
    test_jit_ret_value=$?
  fi

  if [ "${LESMA_WATCHDOG_TIMED_OUT}" -eq 1 ]; then
    printf 'fail-timeout %s\n' "${name}" >"${result_file}"
  elif [ "${test_jit_ret_value}" -ne "${test_expected_ret_value}" ] && [ "${expected_to_fail}" -eq 0 ]; then
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
  local file="${2:-}"
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
    if [ -n "${file}" ]; then
      failed_tests+=("${file}"$'\t'"run failed (expected $3, got $4)")
    else
      failed_tests+=("$2: run failed (expected $3, got $4)")
    fi
    ;;
  fail-expect)
    fail_count=$((fail_count + 1))
    printf 'Testing %s\n' "$2"
    printf '  Run succeeded but was expected to fail\n'
    if [ -n "${file}" ]; then
      failed_tests+=("${file}"$'\t'"expected failure but run succeeded")
    else
      failed_tests+=("$2: expected failure but run succeeded")
    fi
    ;;
  fail-timeout)
    fail_count=$((fail_count + 1))
    printf 'Testing %s\n' "$2"
    printf '  Timed out after %ss (LESMA_TEST_TIMEOUT)\n' "${LESMA_TEST_TIMEOUT}"
    if [ -n "${file}" ]; then
      failed_tests+=("${file}"$'\t'"timed out after ${LESMA_TEST_TIMEOUT}s")
    else
      failed_tests+=("$2: timed out after ${LESMA_TEST_TIMEOUT}s")
    fi
    ;;
  *)
    printf 'Internal error: bad result line: %s\n' "${line}" >&2
    exit 1
    ;;
  esac
}

# Run tests with up to NUM_JOBS concurrent compiler processes, but print each result as soon as
# it is the next line in file order (stable ordering, progressive output).
run_suite_parallel() {
  local tmpdir="$1"
  local expected_to_fail="$2"
  shift 2
  local -a files=("$@")
  local n=${#files[@]}
  local next_to_start=0
  local printed=0
  local -a pids=()
  local file rp line newp pid

  if [ "${n}" -eq 0 ]; then
    return 0
  fi

  while [ "${printed}" -lt "${n}" ]; do
    newp=()
    for pid in "${pids[@]}"; do
      if kill -0 "${pid}" 2>/dev/null; then
        newp+=("${pid}")
      fi
    done
    pids=("${newp[@]}")

    while [ "${next_to_start}" -lt "${n}" ] && [ "${#pids[@]}" -lt "${NUM_JOBS}" ]; do
      file="${files[$next_to_start]}"
      (
        rp="${tmpdir}/$(result_path_for_file "${file}")"
        run_single_test "${file}" "${expected_to_fail}" "${compiler_path}" "${rp}"
      ) &
      pids+=($!)
      next_to_start=$((next_to_start + 1))
    done

    while [ "${printed}" -lt "${n}" ]; do
      file="${files[$printed]}"
      rp="${tmpdir}/$(result_path_for_file "${file}")"
      if [ ! -f "${rp}" ]; then
        break
      fi
      line=$(cat "${rp}")
      process_result_line "${line}" "${file}"
      printed=$((printed + 1))
    done

    if [ "${printed}" -ge "${n}" ]; then
      break
    fi
    sleep 0.05
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

run_suite_parallel "${TMPDIR_RESULTS}" 1 "${failure_files[@]}"

printf 'Tests:\n'
printf '  fail:    %d\n' "${fail_count}"
printf '  success: %d\n' "${success_count}"
if [ "${fail_count}" -gt 0 ]; then
  printf 'Failed tests summary:\n'
  for failed in "${failed_tests[@]}"; do
    case "${failed}" in
    *$'\t'*)
      rel=$(repo_relative_path "${failed%%$'\t'*}")
      msg="${failed#*$'\t'}"
      printf '  - %s: %s\n' "${rel}" "${msg}"
      ;;
    *)
      printf '  - %s\n' "${failed}"
      ;;
    esac
  done
fi
if [ "${fail_count}" -gt 0 ]; then
  exit 1
fi
exit 0