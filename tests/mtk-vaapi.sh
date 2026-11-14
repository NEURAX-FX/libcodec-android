#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Opt-in on the MTK Codec2 device, with the container's existing policy.
set -eu
build=${1:-./build}
render=${2:-/dev/dri/renderD128}
[ -f "$build/codec2_drv_video.so" ] || { echo 'FAIL: Codec2 VA driver missing' >&2; exit 1; }
[ -x "$build/c2-prime-test" ] || { echo 'FAIL: PRIME import test missing' >&2; exit 1; }
[ -c "$render" ] || { echo "FAIL: no DRM render node at $render" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
driver_path=$(realpath "$build")
prime_decode() {
    LIBVA_DRIVER_NAME=codec2 LIBVA_DRIVERS_PATH="$driver_path" timeout 45s \
        ffmpeg -nostdin -v error -xerror -init_hw_device "drm=prime:$render" -filter_hw_device prime \
        -threads:v 1 -hwaccel vaapi -hwaccel_device "$render" -hwaccel_output_format vaapi \
        -i "$1" -vf 'hwmap=mode=read+direct,format=drm_prime,hwdownload,format=yuv420p' \
        -fps_mode passthrough -c:v rawvideo -threads:v 1 -pix_fmt yuv420p -f rawvideo "$2"
}
ffmpeg -nostdin -v error -f lavfi -i 'testsrc2=size=162x106:rate=24' -frames:v 29 \
    -c:v libx264 -threads:v 1 -profile:v high -x264-params 'bframes=3:keyint=12:min-keyint=12:scenecut=0:slices=2' \
    -pix_fmt yuv420p "$work/avc.mp4"
ffmpeg -nostdin -v error -xerror -threads:v 1 -i "$work/avc.mp4" -fps_mode passthrough \
    -c:v rawvideo -threads:v 1 -pix_fmt yuv420p -f rawvideo "$work/reference.yuv"
# hwdownload fails on software frames; -xerror fails the run on decode errors.
LIBVA_DRIVER_NAME=codec2 LIBVA_DRIVERS_PATH="$driver_path" timeout 45s \
    ffmpeg -nostdin -v error -xerror -threads:v 1 -hwaccel vaapi -hwaccel_device "$render" \
    -hwaccel_output_format vaapi -i "$work/avc.mp4" -vf 'hwdownload,format=yuv420p' \
    -fps_mode passthrough -c:v rawvideo -threads:v 1 -pix_fmt yuv420p -f rawvideo "$work/va.yuv"
[ "$(wc -c < "$work/reference.yuv")" -eq $((29 * 162 * 106 * 3 / 2)) ]
cmp "$work/reference.yuv" "$work/va.yuv"
printf '%s\n' 'H.264 High: 29 frames at 162x106 (B frames and multislice), VA-API I420 matches reference.'

# Test direct VA -> DRM PRIME mapping before downloading from the DMA-BUF.
prime_decode "$work/avc.mp4" "$work/prime.yuv"
cmp "$work/reference.yuv" "$work/prime.yuv"
printf '%s\n' 'H.264 High: direct DRM PRIME mapping and download match reference.'

LIBVA_DRIVER_NAME=codec2 LIBVA_DRIVERS_PATH="$driver_path" timeout 45s \
    "$build/c2-prime-test" "$work/avc.mp4" "$render"

# HEVC uses a different coded-size alignment. Keep a padded, closed-GOP sample
# with B pictures to catch lost tail frames and incorrect surface association.
ffmpeg -nostdin -v error -f lavfi -i 'testsrc2=size=162x106:rate=24' -frames:v 29 \
    -c:v libx265 -threads:v 1 -preset fast -pix_fmt yuv420p \
    -x265-params 'frame-threads=1:pools=none:log-level=error:bframes=3:keyint=12:min-keyint=12:scenecut=0:open-gop=0' \
    "$work/hevc.mp4"
ffmpeg -nostdin -v error -xerror -threads:v 1 -i "$work/hevc.mp4" -fps_mode passthrough \
    -c:v rawvideo -threads:v 1 -pix_fmt yuv420p -f rawvideo "$work/hevc-reference.yuv"
prime_decode "$work/hevc.mp4" "$work/hevc-prime.yuv"
[ "$(wc -c < "$work/hevc-reference.yuv")" -eq $((29 * 162 * 106 * 3 / 2)) ]
cmp "$work/hevc-reference.yuv" "$work/hevc-prime.yuv"
printf '%s\n' 'HEVC Main: 29 frames at 162x106 (B frames), direct DRM PRIME mapping and download match reference.'
