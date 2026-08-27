#!/usr/bin/env bash
set -euo pipefail

test_binary="${TMPDIR:-/tmp}/ygsoul-content-storage-policy-test"
c++ -std=c++17 -Wall -Wextra -Werror -I. scripts/test_content_storage_policy.cpp -o "$test_binary"
"$test_binary"
echo "ContentStorage policy passed"
