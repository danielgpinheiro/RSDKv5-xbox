#!/usr/bin/env python3
# Xbox ADPCM encoder (mono) — ported from es-xbox-adpcm-tool's XboxAdpcmCodec.cs
# (block-accurate reference) + the xboxdevwiki spec. Host-side offline tool.
# Emits a proper Xbox ADPCM WAV: fmt tag 0x0069, blockAlign 36, fact chunk.
import sys, os, struct, wave, glob

INDEX_TABLE = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8]
STEP_TABLE = [
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,
    337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
    2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
    15289,16818,18500,20350,22385,24623,27086,29794,32767]
BLOCK_SAMPLES = 64
BLOCK_BYTES = 36

def clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)

def find_initial_index(samples, start):
    avg = 0; cnt = 0
    for k in range(1, 8):
        si = start + k
        if si >= len(samples): break
        avg += abs(samples[si] - samples[si-1]); cnt += 1
    if cnt: avg //= cnt
    best = 0; bd = abs(STEP_TABLE[0] - avg)
    for i in range(1, len(STEP_TABLE)):
        d = abs(STEP_TABLE[i] - avg)
        if d < bd: bd = d; best = i
    return best

def encode_nibble(val, st):  # st = [index, step, predictor]
    valpred = st[2]; step = st[1]
    diff = val - valpred
    sign = 8 if diff < 0 else 0
    if sign: diff = -diff
    delta = 0; vpdiff = step >> 3
    if diff >= step: delta = 4; diff -= step; vpdiff += step
    step >>= 1
    if diff >= step: delta |= 2; diff -= step; vpdiff += step
    step >>= 1
    if diff >= step: delta |= 1; vpdiff += step
    valpred = valpred - vpdiff if sign else valpred + vpdiff
    valpred = clamp16(valpred)
    delta |= sign
    st[0] += INDEX_TABLE[delta]
    st[0] = 0 if st[0] < 0 else (88 if st[0] > 88 else st[0])
    st[1] = STEP_TABLE[st[0]]; st[2] = valpred
    return delta

def encode_mono(samples):
    total = len(samples)
    blocks = (total + BLOCK_SAMPLES - 1) // BLOCK_SAMPLES
    out = bytearray()
    for blk in range(blocks):
        start = blk * BLOCK_SAMPLES
        pred = samples[start] if start < total else (samples[-1] if total else 0)
        idx = find_initial_index(samples, start)
        st = [idx, STEP_TABLE[idx], pred]
        out += struct.pack('<hh', pred, idx)  # int16 predictor, int16 index
        for group in range(8):
            pack = 0
            for i in range(8):
                si = start + group * 8 + i
                s = samples[si] if si < total else st[2]  # pad with predictor
                out_code = encode_nibble(s, st)
                pack |= (out_code & 0xF) << (i * 4)
            out += struct.pack('<I', pack)
    return bytes(out), total, blocks

def decode_mono(data):  # for verification
    out = []
    nblk = len(data) // BLOCK_BYTES
    p = 0
    for _ in range(nblk):
        pred = struct.unpack_from('<h', data, p)[0]; p += 2
        idx = struct.unpack_from('<h', data, p)[0]; p += 2
        idx = 0 if idx < 0 else (88 if idx > 88 else idx)
        st = [idx, STEP_TABLE[idx], pred]
        for group in range(8):
            pack = struct.unpack_from('<I', data, p)[0]; p += 4
            for j in range(8):
                code = pack & 0xF; pack >>= 4
                delta = st[1] >> 3
                if code & 4: delta += st[1]
                if code & 2: delta += st[1] >> 1
                if code & 1: delta += st[1] >> 2
                if code & 8: delta = -delta
                v = clamp16(st[2] + delta)
                st[0] += INDEX_TABLE[code]
                st[0] = 0 if st[0] < 0 else (88 if st[0] > 88 else st[0])
                st[1] = STEP_TABLE[st[0]]; st[2] = v
                out.append(v)
    return out

def read_pcm_wav(path):
    # Manual RIFF parser: handles PCM (fmt 1, 8/16-bit) and IEEE float (fmt 3, 32-bit).
    d = open(path, 'rb').read()
    if d[:4] != b'RIFF' or d[8:12] != b'WAVE':
        raise ValueError("not a WAV: %s" % path)
    p = 12; fmt = None; data = None
    while p + 8 <= len(d):
        cid = d[p:p+4]; sz = struct.unpack_from('<I', d, p+8-4)[0]; body = p+8
        if cid == b'fmt ':
            tag, ch, rate, _abps, _ba, bits = struct.unpack_from('<HHIIHH', d, body)
            fmt = (tag, ch, rate, bits)
        elif cid == b'data':
            data = d[body:body+sz]
        p = body + sz + (sz & 1)
    if fmt is None or data is None:
        raise ValueError("missing fmt/data: %s" % path)
    tag, ch, rate, bits = fmt
    # RSDK mislabels some SFX with fmt tag 0x0003 but bits=16 blockAlign=2 and S16 data,
    # and the engine reads by BIT DEPTH (ignoring the tag). Match that: decide by bits.
    if bits == 16:
        n = len(data) // 2
        interleaved = list(struct.unpack('<%dh' % n, data))
    elif bits == 8:
        interleaved = [((b - 128) << 8) for b in data]
    elif bits == 32 and tag == 3:
        n = len(data) // 4
        interleaved = [clamp16(int(round(x * 32767.0))) for x in struct.unpack('<%df' % n, data)]
    else:
        raise ValueError("unsupported fmt tag=%d bits=%d: %s" % (tag, bits, path))
    if ch == 1:
        samples = interleaved
    elif ch == 2:
        # APU SFX voices are mono; downmix L/R (average). Drops a stray trailing sample.
        m = len(interleaved) // 2
        samples = [(interleaved[2*i] + interleaved[2*i+1]) // 2 for i in range(m)]
    else:
        raise ValueError("unsupported channel count %d: %s" % (ch, path))
    return samples, rate

def write_xbadpcm_wav(path, adpcm, total_samples, rate):
    blocks = len(adpcm) // BLOCK_BYTES
    avg_bps = (rate * BLOCK_BYTES) // BLOCK_SAMPLES
    fmt = struct.pack('<HHIIHHH', 0x0069, 1, rate, avg_bps, BLOCK_BYTES, 4, 2) + struct.pack('<H', BLOCK_SAMPLES)
    fact = struct.pack('<I', total_samples)
    body = (b'WAVE'
            + b'fmt ' + struct.pack('<I', len(fmt)) + fmt
            + b'fact' + struct.pack('<I', 4) + fact
            + b'data' + struct.pack('<I', len(adpcm)) + adpcm)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(body)) + body)

def convert(src, dst):
    samples, rate = read_pcm_wav(src)
    adpcm, total, blocks = encode_mono(samples)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    write_xbadpcm_wav(dst, adpcm, total, rate)
    return total, blocks, len(adpcm)

if __name__ == '__main__':
    if sys.argv[1] == '--verify':
        # roundtrip RMS check on one file
        samples, rate = read_pcm_wav(sys.argv[2])
        adpcm, total, blocks = encode_mono(samples)
        dec = decode_mono(adpcm)
        n = min(len(samples), len(dec))
        err = sum((samples[i]-dec[i])**2 for i in range(n)) / max(n,1)
        import math
        print("samples=%d blocks=%d adpcm_bytes=%d (pcm_bytes=%d, ratio=%.2fx) rms_err=%.1f rate=%d"
              % (total, blocks, len(adpcm), len(samples)*2, (len(samples)*2)/max(len(adpcm),1), math.sqrt(err), rate))
    elif sys.argv[1] == '--batch':
        srcroot, dstroot = sys.argv[2], sys.argv[3]
        wavs = sorted(glob.glob(os.path.join(srcroot, '**', '*.wav'), recursive=True))
        tot_pcm = tot_adpcm = 0; nfiles = 0
        for src in wavs:
            rel = os.path.relpath(src, srcroot)
            dst = os.path.join(dstroot, rel)
            try:
                total, blocks, abytes = convert(src, dst)
                tot_pcm += total*2; tot_adpcm += abytes; nfiles += 1
            except Exception as e:
                print("SKIP %s: %s" % (rel, e))
        print("converted %d files: PCM %.1f MB -> ADPCM %.1f MB (%.2fx)"
              % (nfiles, tot_pcm/1e6, tot_adpcm/1e6, tot_pcm/max(tot_adpcm,1)))
    else:
        convert(sys.argv[1], sys.argv[2])
