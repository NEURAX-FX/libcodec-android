#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
bin=${1:-./build/c2-list}
output=$(timeout 20s "$bin" --hardware-only)
printf '%s\n' "$output"
printf '%s\n' "$output" | awk -F '\t' '
    NR == 1 && $0 == "Service: android.hardware.media.c2@1.2::IComponentStore/default" { store = 1 }
    NR == 3 && $0 == "  android.hardware.media.c2@1.2::IComponentStore" { v12 = 1 }
    NR == 4 && $0 == "  android.hardware.media.c2@1.1::IComponentStore" { v11 = 1 }
    NR == 5 && $0 == "  android.hardware.media.c2@1.0::IComponentStore" { v10 = 1 }
    NR == 6 && $0 == "  android.hidl.base@1.0::IBase" { base = 1 }
    NF >= 7 && $1 != "NAME" {
        if ($2 != "video" || $3 != "decoder" || ($4 != "video/avc" && $4 != "video/hevc") ||
            $5 != "hardware") exit 1
        secure = ($7 ~ /(^| )secure( |$)/)
        if ($1 == "c2.mtk.avc.decoder" && $4 == "video/avc" && !secure) avc = 1
        if ($1 == "c2.mtk.hevc.decoder" && $4 == "video/hevc" && !secure) hevc = 1
        if ($1 == "c2.mtk.avc.decoder.secure" && $4 == "video/avc" && secure) secure_avc = 1
        if ($1 == "c2.mtk.hevc.decoder.secure" && $4 == "video/hevc" && secure) secure_hevc = 1
    }
    END { if (!(store && v12 && v11 && v10 && base && avc && hevc && secure_avc && secure_hevc)) exit 1 }
'
