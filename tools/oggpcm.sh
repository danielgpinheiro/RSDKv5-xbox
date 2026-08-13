#!/usr/bin/env bash
# oggpcm.sh — convert Sonic Mania's music to loose streamable PCM for the Xbox.
#
# CREDIT: the format and approach are based on the Dreamcast port of RSDKv5 /
# Sonic Mania — specifically its offline asset pipeline
# (dreamcast/music_step_1_ogg_to_pcm.sh, OGG -> raw PCM) and the streamed
# 22050 Hz / 8-bit / stereo PCM its audio backend uses (KallistiOSStream.cpp).
# Extraction below reuses that port's dreamcast/rsdkv5_extract.py.
#
# The nxaudio music path (RSDK/Audio/Audio.cpp: LoadStreamLoosePCM /
# UpdateStreamBufferLoosePCM) streams raw PCM straight from disc instead of
# CPU-decoding OGG with stb_vorbis — the Dreamcast port's approach. Format is
# **22050 Hz, 8-bit unsigned, stereo, interleaved, headerless** (matches the
# engine's reader), ~44 KB/s. Files ship loose at D:\MusicPCM\<name>.pcm (staged
# from the project MusicPCM/ dir by Makefile.nxdk); the engine maps the game's
# requested "Data/Music/<name>.ogg" -> "<name>.pcm" and falls back to the packed
# OGG when a loose file is absent.
#
# Extract the OGGs first (read-only) with an RSDKv5 decomp tool — e.g. the Dreamcast
# port's dreamcast/rsdkv5_extract.py under python3:
#   python3 rsdkv5_extract.py /path/to/Data.rsdk /tmp/rsdk_src   # -> /tmp/rsdk_src/Music/*.ogg
# then:
#   tools/oggpcm.sh /tmp/rsdk_src/Music        # -> MusicPCM/*.pcm
#   tools/oggpcm.sh /tmp/rsdk_src/Music out    # explicit output dir
set -euo pipefail

command -v ffmpeg >/dev/null 2>&1 || { echo "error: ffmpeg not found in PATH" >&2; exit 1; }

SRC="${1:-}"
DST="${2:-MusicPCM}"
[ -n "$SRC" ] && [ -d "$SRC" ] || { echo "usage: tools/oggpcm.sh <src-ogg-dir> [dst-dir]" >&2; exit 1; }

mkdir -p "$DST"
shopt -s nullglob nocaseglob
srcs=("$SRC"/*.ogg)
[ "${#srcs[@]}" -gt 0 ] || { echo "no .ogg files in $SRC" >&2; exit 1; }

n=0
for f in "${srcs[@]}"; do
  base="$(basename "${f%.*}")"
  out="$DST/$base.pcm"
  echo "[ PCM ] $(basename "$f") -> $base.pcm"
  # -f u8 = headerless unsigned 8-bit; -ar 22050 -ac 2 = 22.05kHz stereo.
  ffmpeg -hide_banner -loglevel error -y -i "$f" -ar 22050 -ac 2 -f u8 "$out"
  n=$((n + 1))
done
echo "converted $n track(s) -> $DST/"
