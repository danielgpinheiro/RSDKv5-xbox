# tools/

## xbadpcm.py — Xbox ADPCM SFX encoder (host-side)

Converts Sonic Mania's SFX to **Xbox ADPCM** (4-bit, fmt tag `0x0069`, 36-byte mono
blocks) for the nxaudio backend. ADPCM SFX are ~1/4 the RAM of S16 PCM, shrinking the
`DATASET_SFX` pool on the 64 MB console. They ship as **loose files** at `D:\SoundFXAD\`
(no `Data.rsdk` repack); the engine loads them in preference to the pack's PCM WAVs and
falls back to the pack if a loose file is absent (`LoadSfxADPCMLoose` in `Audio.cpp`).

Algorithm ported from es-xbox-adpcm-tool's `XboxAdpcmCodec.cs` + the xboxdevwiki spec
(standard IMA; per-block header = int16 predictor + int16 index, then 8×uint32 packed
nibbles = 64 samples). Handles PCM 8/16-bit, RSDK's mislabeled fmt-tag-3 quirk (decide by
bit depth), and stereo (downmix to mono — the APU SFX voices are mono).

### Regenerate `SoundFXAD/` (gitignored)
```bash
# 1) Extract SoundFX/ from Data.rsdk (read-only) with the RSDKv5 decomp tool, e.g. the
#    Dreamcast port's dreamcast/rsdkv5_extract.py under python3:
python3 rsdkv5_extract.py /path/to/Data.rsdk /tmp/rsdk_src        # -> /tmp/rsdk_src/SoundFX/...
# 2) Convert all SFX -> Xbox ADPCM into SoundFXAD/ (mirrors the SoundFX/ subdir layout):
python3 tools/xbadpcm.py --batch /tmp/rsdk_src/SoundFX SoundFXAD
# 3) Build; Makefile.nxdk stages SoundFXAD/ -> ISO as D:\SoundFXAD\ automatically.
```
`--verify <file.wav>` prints a roundtrip RMS check + compression ratio for one file.

To keep a specific SFX at full PCM quality (ADPCM is grainy on very quiet tails), just
delete its file from `SoundFXAD/` — the engine falls back to the pack PCM for it.

## mkmpg.sh — FMV re-encoder (host-side)

Re-encodes cutscene videos to **MPEG-1** for the Xbox `pl_mpeg` player (`RSDK/Graphics/Video.cpp`),
which replaced Theora on Xbox. Output is an MPEG **Program Stream** (container `mpeg`, video-only)
— pl_mpeg's demuxer needs a PS, not a raw `mpeg1video` elementary stream. Files ship loose at
`D:\Videos\<name>.mpg`; the engine maps the game's requested `<name>.ogv` → `<name>.mpg` and skips
the FMV if the file is absent. `Makefile.nxdk` stages only `Videos/*.mpg` into the ISO.

```bash
tools/mkmpg.sh                       # batch: every Videos/*.ogv -> Videos/*.mpg
tools/mkmpg.sh in.ogv                # single -> Videos/in.mpg
SCALE=512:256 BITRATE=1500k tools/mkmpg.sh   # override size / bitrate
```
Knobs: `BITRATE` (default 1500k), `SCALE` (`W:H`, default keep source), `FPS` (default keep source;
must be a legal MPEG-1 rate — 24000/1001, 24, 25, 30000/1001, 30, 50, 60000/1001, 60). Source frame
rate is preserved automatically. pl_mpeg's smaller decode footprint also fits the full-res 1024×512
cutscenes that the Theora decoder OOMed on — encode those with `SCALE=1024:512` to try.

## oggpcm.sh — music → loose streamable PCM (host-side)

Converts Sonic Mania's music to **22050 Hz / 8-bit unsigned / stereo / headerless raw PCM**, shipped
loose at `D:\MusicPCM\<name>.pcm`. The nxaudio backend streams these straight from disc
(`LoadStreamLoosePCM` / `UpdateStreamBufferLoosePCM` in `RSDK/Audio/Audio.cpp`, 2× upsampled to the
44.1 kHz float ring) instead of CPU-decoding OGG with stb_vorbis — the Dreamcast approach. This drops
the Vorbis CPU cost and shrinks `DATASET_MUS` (4.5 MB → 1 MB). The engine maps the game's requested
`Data/Music/<name>.ogg` → `<name>.pcm` and falls back to the packed OGG when a loose file is absent.

> Like `SoundFXAD/`, `MusicPCM/` is **expected present** for the NXAUDIO build: the shrunken
> `DATASET_MUS` can't hold a whole large OGG, so a *missing* loose track degrades to silence (never a
> crash). Cost: ~44 KB/s ≈ 155 MB for the full soundtrack.

### Regenerate `MusicPCM/` (gitignored)
```bash
# 1) Extract Music/ from Data.rsdk (read-only) — e.g. the Dreamcast port's tool under python3:
python3 rsdkv5_extract.py /path/to/Data.rsdk /tmp/rsdk_src     # -> /tmp/rsdk_src/Music/*.ogg
# 2) Convert every track -> 22050/8-bit/stereo raw PCM into MusicPCM/:
tools/oggpcm.sh /tmp/rsdk_src/Music MusicPCM
# 3) Build; Makefile.nxdk stages MusicPCM/ -> ISO as D:\MusicPCM\ automatically.
```
