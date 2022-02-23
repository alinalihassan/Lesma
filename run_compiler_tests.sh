#!/bin/bash

echo "Compiling source code"

# Build source
if ! { cmake -B build -DCMAKE_BUILD_TYPE=Release &> /tmp/compile_output.log &&
       cmake --build build --config Release &> /tmp/compile_output.log ; }; then
	echo "Failed to build the compiler"
	exit 1
fi

echo "Sources built"
echo ""

fail_count=0
success_count=0

# We currently cannot test compiler errors, since we assume all runs need an exit code > 0
# We assume compiler errors throw error codes < 100 for now
for file in tests/*.les; do
	name=$(basename -s .les "${file}")
	echo "Testing ${name}"
	build/lesma run "${file}" &> /tmp/compile_output.log
  test_ret_value=$?
  test_expected_ret_value=$(head -n 1 "$file" | cut -b 6-)
  if [ "$test_ret_value" -ne "$test_expected_ret_value" ]; then
    fail_count=$((fail_count + 1))
    echo "  Run failed, expected $test_expected_ret_value, got $test_ret_value"
  else
    success_count=$((success_count + 1))
    echo "  Run succeeded"
  fi
done

echo "Tests:"
echo "  fail:    ${fail_count}"
echo "  success: ${success_count}"
exit ${fail_count}
