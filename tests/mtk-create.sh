#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
bin=${1:-./build/c2-create}
if [ ! -x "$bin" ]; then echo 'FAIL: c2-create executable is missing'; exit 1; fi
output=$(timeout 20s "$bin" --size 128x96 --name c2.mtk.avc.decoder)
printf '%s\n' "$output"
printf '%s\n' "$output" | awk '
    NR == 1 && $0 == "Created: c2.mtk.avc.decoder" { ++matched }
    NR == 2 && /^Supported parameters: [1-9][0-9]*$/ { ++matched }
    NR == 3 && $0 == "Configured: 128x96" { ++matched }
    NR == 4 && $0 == "Started" { ++matched }
    NR == 5 && $0 == "Stopped" { ++matched }
    NR == 6 && $0 == "Released" { ++matched }
    END { if (matched != 6) exit 1 }
'
