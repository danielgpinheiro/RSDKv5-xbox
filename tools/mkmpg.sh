#!/usr/bin/env bash
# mkmpg.sh — re-encode FMV cutscenes to MPEG-1 for the Xbox pl_mpeg player.
#
# CREDIT: the MPEG-1 + pl_mpeg FMV approach is based on the Dreamcast port of
# RSDKv5 / Sonic Mania — its offline video pipeline (dreamcast/video_script.sh) and
# the pl_mpeg decoder it plays back with (RSDK/Graphics/KallistiOS/mpeg.c). This
# script is the Xbox-side equivalent, producing MPEG Program Streams for our
# pl_mpeg-based player.
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
#   TRIM     auto-trim a dead leading hold  (1 = on, default; 0 = off)
#
# Dead-lead-in trim (TRIM=1): some sources open on a held title card stored as a
# couple of sparse VFR frames followed by a multi-second timestamp gap — Mania.ogv
# is the one such file here (only ~2 frames before 7.2s, a frozen shot of Sonic's
# shoes). Re-encoding to constant-rate MPEG-1 would faithfully fill that gap with
# duplicate frames, baking ~7s of static into the FMV. The intro music does NOT
# expect that hold, so we detect the leading gap and start the encode at the first
# real frame past it, opening the FMV on motion. Dense constant-rate sources have no
# such leading gap and are encoded from 0 untouched.
set -euo pipefail

command -v ffmpeg >/dev/null 2>&1 || { echo "error: ffmpeg not found in PATH" >&2; exit 1; }
command -v ffprobe >/dev/null 2>&1 || { echo "error: ffprobe not found in PATH" >&2; exit 1; }

BITRATE="${BITRATE:-1500k}"
SCALE="${SCALE:-}"
FPS="${FPS:-}"
TRIM="${TRIM:-1}"

# Print the pts (seconds) of the first frame past a *leading* held-frame gap, or
# nothing if the source opens densely. "Leading" = the gap must start within the
# first LEAD_MAX seconds (so a legitimate mid-content gap can't trigger a trim);
# GAP_MIN is the smallest gap treated as a dead hold.
detect_lead_trim() {
  # Note: awk must NOT `exit` on first match — closing the pipe early SIGPIPEs
  # ffprobe, which `set -o pipefail` would turn into a script abort. Consume all
  # input (bounded to ~20s by -read_intervals) and print only the first match.
  ffprobe -v error -select_streams v:0 -show_entries frame=pts_time -of csv=p=0 \
    -read_intervals "%+20" "$1" 2>/dev/null | \
  awk -v gap_min=1.0 -v lead_max=1.5 '
    { t=$1+0; if (!done && NR>1 && prev<lead_max && (t-prev)>gap_min) { printf "%.6f\n", t; done=1 } prev=t }'
}

encode_one() {
  local src="$1" dst="$2"
  local vf=() rate=() ss=()
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
  # Drop a dead leading hold. -ss before -i seeks (accurate_seek is on by default)
  # so output starts at the first real frame with its PTS rebased to ~0 — the FMV
  # opens on motion and stays aligned with music that starts at 0.
  local trim=""
  if [ "$TRIM" = "1" ]; then
    trim="$(detect_lead_trim "$src")"
    [ -n "$trim" ] && ss=(-ss "$trim")
  fi
  echo "[ MPEG1 ] $(basename "$src") -> $(basename "$dst")  (b:v=$BITRATE ${SCALE:+scale=$SCALE }${FPS:+fps=$FPS}${trim:+trim@${trim}s })"
  # ${arr[@]+"${arr[@]}"} keeps this safe under `set -u` on bash 3.2 (macOS) when empty.
  ffmpeg -hide_banner -loglevel error -y ${ss[@]+"${ss[@]}"} -i "$src" \
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
