#!/bin/sh
# Runs the heap logic unit tests on Linux / WSL / macOS (needs clang).
cd "$(dirname "$0")" || exit 1
clang -O1 -w -fms-extensions -fdeclspec -o /tmp/scf_test test_heap.c || exit 1
/tmp/scf_test
