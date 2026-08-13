#!/usr/bin/env bash
# mkmpg.sh — re-encode FMV cutscenes to MPEG-1 for the Xbox pl_mpeg player.
#
# The Xbox FMV path (RSDK/Graphics/Video.cpp) decodes MPEG-1 via pl_mpeg, replacing
# Theora. pl_mpeg's high-level demuxer expects an MPEG **Program Stream** (container
# "mpeg", NOT a raw mpeg1video elementary stream), video-only. Files ship loose at
# D:\Videos\<name>.mpg (staged from the project Videos/ dir by Makefile.nxdk); the
# engine maps the game's requested "<name>.ogv" -> "<name>.mpg".
#
# Usage:
#   tools/mkmpg.sh                      # batch: every Videos/*.ogv -> Videos/*.mpg
#   tools/mkmpg.sh in.ogv               # -> Videos/in.mpg
#   tools/mkmpg.sh in.ogv out.mpg       # explicit output
#   SCALE=512:256 BITRATE=1500k tools/mkmpg.sh   # override size / bitrate
#
# Env knobs:
#   BITRATE  video bitrate            (default 1500k)
#   SCALE    "W:H" ffmpeg scale       (default: keep source size)
#   FPS      output frame rate        (default: keep source rate; must be a legal
#                                      MPEG-1 rate: 24000/1001, 24, 25, 30000/1001,
#                                      30, 50, 60000/1001, 60)
set -euo pipefail

command -v ffmpeg >/dev/null 2>&1 || { echo "error: ffmpeg not found in PATH" >&2; exit 1; }

BITRATE="${BITRATE:-1500k}"
SCALE="${SCALE:-}"
FPS="${FPS:-}"

encode_one() {
  local src="$1" dst="$2"
  local vf=() rate=()
  [ -n "$SCALE" ] && vf=(-vf "scale=${SCALE}:flags=lanczos")
  if [ -n "$FPS" ]; then
    rate=(-r "$FPS")
  else
    # Preserve the source rate (MPEG-1 only accepts a fixed set; ffmpeg maps
    # 24000/1001 etc. to the right frame_rate_code).
    local src_fps
    src_fps="$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate -of default=nw=1:nk=1 "$src" 2>/dev/null || true)"
    [ -n "$src_fps" ] && [ "$src_fps" != "0/0" ] && rate=(-r "$src_fps")
  fi
  echo "[ MPEG1 ] $(basename "$src") -> $(basename "$dst")  (b:v=$BITRATE ${SCALE:+scale=$SCALE }${FPS:+fps=$FPS})"
  # ${arr[@]+"${arr[@]}"} keeps this safe under `set -u` on bash 3.2 (macOS) when empty.
  ffmpeg -hide_banner -loglevel error -y -i "$src" \
    -an -c:v mpeg1video -b:v "$BITRATE" ${rate[@]+"${rate[@]}"} ${vf[@]+"${vf[@]}"} \
    -f mpeg "$dst"
}

if [ "$#" -eq 0 ]; then
  shopt -s nullglob
  vids=(Videos/*.ogv Videos/*.mp4 Videos/*.mov Videos/*.avi)
  [ "${#vids[@]}" -gt 0 ] || { echo "no source videos found in Videos/" >&2; exit 1; }
  for src in "${vids[@]}"; do
    encode_one "$src" "Videos/$(basename "${src%.*}").mpg"
  done
elif [ "$#" -eq 1 ]; then
  encode_one "$1" "Videos/$(basename "${1%.*}").mpg"
else
  encode_one "$1" "$2"
fi
echo "done."
