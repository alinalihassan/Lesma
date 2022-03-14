#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

fail_count=0
success_count=0

# We currently cannot test compiler errors, since we assume all runs need an exit code > 0
# We assume compiler errors throw error codes < 100 for now
for file in "$SCRIPT_DIR"/tests/lesma/*.les; do
	name=$(basename -s .les "${file}")
	echo "Testing ${name}"
	"$SCRIPT_DIR"/build/lesma compile "${file}" &> /tmp/compile_output.log
	test_aot_ret_value=$?
	"$SCRIPT_DIR"/build/lesma run "${file}" &> /tmp/compile_output.log
  test_jit_ret_value=$?
  test_expected_ret_value=$(head -n 1 "$file" | cut -b 6-)
  if [ "$test_jit_ret_value" -ne "$test_expected_ret_value" ]; then
    fail_count=$((fail_count + 1))
    echo "  Run failed, expected $test_expected_ret_value, got $test_jit_ret_value"
  elif [ $test_aot_ret_value -ne 0 ]; then
    fail_count=$((fail_count + 1))
    echo "  Compilation failed, expected 0, got $test_aot_ret_value"
  else
    success_count=$((success_count + 1))
    echo "  Run succeeded"
  fi
done

echo "Tests:"
echo "  fail:    ${fail_count}"
echo "  success: ${success_count}"
exit ${fail_count}
