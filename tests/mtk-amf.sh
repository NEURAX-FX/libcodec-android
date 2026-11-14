#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

build=$(realpath "${1:-build-amf}")
[ -f "$build/libamfrt64.so.1" ] || { echo 'FAIL: Codec2 AMF library missing' >&2; exit 1; }
ffmpeg=${FFMPEG:-ffmpeg}
umask 077
ulimit -c 0
dir=$(mktemp -d)
trap 'rm -rf -- "$dir"' EXIT

# More than one ring of work slots, nontrivial strides, irregular PTS and lookahead.
frames=40
"$ffmpeg" -nostdin -v error -xerror -f lavfi -i testsrc2=size=162x106:rate=12 \
  -vf 'setpts=PTS+floor(N/3)' -fps_mode vfr -frames:v "$frames" -an \
  -c:v libaom-av1 -cpu-used 8 -lag-in-frames 16 -g 100 -crf 28 -b:v 0 \
  -threads:v 1 -pix_fmt yuv420p "$dir/in-band.mp4"
"$ffmpeg" -nostdin -v error -xerror -i "$dir/in-band.mp4" -map 0:v:0 \
  -c:v copy -bsf:v filter_units=remove_types=1 "$dir/extra-data.mp4"

compare() {
  sample=$1
  filter=$2
  # dav1d's FFmpeg wrapper reads av1C for metadata but needs the sequence
  # header in-band for decoding. The remux changed only its location.
  "$ffmpeg" -nostdin -v error -xerror -c:v libdav1d -i "$dir/in-band.mp4" -map 0:v:0 \
    -vf "$filter" -enc_time_base:v demux -fps_mode passthrough -c:v rawvideo \
    -pix_fmt yuv420p -threads:v 1 -f framemd5 "$dir/$sample.reference"
  LD_LIBRARY_PATH="$build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 35s "$ffmpeg" -nostdin -v error -xerror -c:v av1_amf -i "$dir/$sample.mp4" -map 0:v:0 \
    -vf "$filter" -enc_time_base:v demux -fps_mode passthrough -c:v rawvideo \
    -pix_fmt yuv420p -threads:v 1 -f framemd5 "$dir/$sample.amf"
  # Frame records include time base (checked separately), PTS, duration, size and pixel hash.
  for backend in reference amf; do
    awk '/^#tb / || /^[^#]/' "$dir/$sample.$backend" > "$dir/$sample.$backend.records"
    test "$(grep -c '^[^#]' "$dir/$sample.$backend.records")" -eq "$frames"
  done
  diff -u "$dir/$sample.reference.records" "$dir/$sample.amf.records"
  printf '%s: %s frames match dav1d (timestamps, durations, and pixel hashes)\n' "$sample" "$frames"
}

compare in-band null
# reverse retains every output frame until EOS. This catches reuse of live frame memory.
compare extra-data reverse
