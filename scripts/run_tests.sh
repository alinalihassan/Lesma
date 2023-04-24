#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

fail_count=0
success_count=0

test_compiler() {
  local file="$1"
  local mode="$2"
  local compiler_path="$3"
  "${compiler_path}" "${mode}" "${file}" &> /dev/null
  return $?
}

test_case() {
  local file="$1"
  local expected_to_fail="$2"
  local compiler_path="$3"
  name=$(basename -s .les "${file}")
  printf "Testing %s\n" "${name}"

  test_expected_ret_value=0

  test_compiler "${file}" "run" "${compiler_path}"
  test_jit_ret_value=$?

  test_compiler "${file}" "compile" "${compiler_path}"
  test_aot_ret_value=$?

  if [ "$test_jit_ret_value" -ne "$test_expected_ret_value" ] && [ "$expected_to_fail" -eq 0 ]; then
    fail_count=$((fail_count + 1))
    printf "  Run failed, expected %d, got %d\n" "$test_expected_ret_value" "$test_jit_ret_value"
  elif [ $test_aot_ret_value -ne 0 ] && [ "$expected_to_fail" -eq 0 ]; then
    fail_count=$((fail_count + 1))
    printf "  Compilation failed, expected 0, got %d\n" "$test_aot_ret_value"
  elif [ "$expected_to_fail" -eq 1 ] && [ "$test_jit_ret_value" -eq "$test_expected_ret_value" ]; then
    fail_count=$((fail_count + 1))
    printf "  Run succeeded but was expected to fail\n"
  else
    success_count=$((success_count + 1))
    printf "  %s\n" "$( [ "$expected_to_fail" -eq 1 ] && echo "Fail" || echo "Run" ) succeeded"
  fi
}

compiler_path="${SCRIPT_DIR}/../build/lesma"
if [ $# -gt 0 ]; then
  compiler_path="$1"
fi

# Test success cases
for file in "$SCRIPT_DIR"/../tests/lesma/success/*.les; do
  test_case "${file}" 0 "${compiler_path}"
done

# Test failure cases
for file in "$SCRIPT_DIR"/../tests/lesma/failure/*.les; do
  test_case "${file}" 1 "${compiler_path}"
done

printf "Tests:\n"
printf "  fail:    %d\n" "${fail_count}"
printf "  success: %d\n" "${success_count}"
exit ${fail_count}
