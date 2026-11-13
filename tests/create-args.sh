#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
bin=$1
assert_usage_error() {
    rc=0
    "$bin" "$@" > /dev/null 2>&1 || rc=$?
    if [ "$rc" -ne 2 ]; then
        printf 'expected usage status 2 from %s, got %s\n' "$*" "$rc" >&2
        exit 1
    fi
}
# A non-Binder device makes any attempted discovery fail deterministically.
assert_usage_error --device /dev/null --name ''
assert_usage_error --device /dev/null --name --params
assert_usage_error --device /dev/null --size 0x96
assert_usage_error --device /dev/null --size 128x
