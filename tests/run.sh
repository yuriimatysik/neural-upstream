#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_binary=$(mktemp "${TMPDIR:-/tmp}/neural-upstream-submission-test.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -pedantic -I src \
    tests/submission_tracker.cpp -o "$test_binary"
"$test_binary"
