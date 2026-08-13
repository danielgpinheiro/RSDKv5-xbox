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
