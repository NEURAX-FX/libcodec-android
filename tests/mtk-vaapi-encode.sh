#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# MT6989 hardware test: negotiation alone does not prove the firmware used the QP.
set -eu
export LIBVA_DRIVER_NAME=codec2
LIBVA_DRIVERS_PATH=$(realpath "${1:-build}")
export LIBVA_DRIVERS_PATH
render=${2:-/dev/dri/renderD128}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
for codec in h264 hevc; do
    for qp in auto 18 38; do
        set --
        [ "$qp" = auto ] || set -- -qp "$qp" -i_qfactor 1 -i_qoffset 0
        timeout 30s ffmpeg -nostdin -v error -xerror -vaapi_device "$render" \
            -f lavfi -i testsrc2=size=640x360:rate=30 -vf format=nv12,hwupload \
            -c:v "${codec}_vaapi" "$@" -frames:v 8 -y "$work/out.mp4"
        frames=$(ffprobe -v error -select_streams v:0 -count_frames \
            -show_entries stream=nb_read_frames -of csv=p=0 "$work/out.mp4")
        [ "$frames" = 8 ]
        [ "$qp" != auto ] || continue
        ffmpeg -nostdin -v info -i "$work/out.mp4" -c:v copy -bsf:v trace_headers -f null - 2>"$work/headers"
        awk -v qp="$qp" '
            / (pic_init_qp_minus26|init_qp_minus26) / { base = 26 + $NF; pps = 1 }
            / slice_qp_delta / { if (!pps || base + $NF != qp) bad = 1; n++ }
            END { exit (bad || n != 8) }
        ' "$work/headers"
    done
    printf '%s: default RC decoded all frames; slice QPs 18 and 38 verified.\n' "$codec"
done
