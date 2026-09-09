#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_binary=$(mktemp "${TMPDIR:-/tmp}/neural-upstream-submission-test.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
for test_source in tests/submission_tracker.cpp tests/descriptor_lifetime.cpp tests/feature_registry.cpp tests/control_state.cpp; do
    "${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -pedantic -I src \
        "$test_source" -o "$test_binary"
    "$test_binary"
done
python3 tests/codec_math.py
