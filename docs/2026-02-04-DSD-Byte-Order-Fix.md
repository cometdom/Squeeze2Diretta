# DSD Byte-Order Bug: Squeezelite stdout LE/BE Mismatch

**Date:** 2026-02-04
**Severity:** Audio quality degradation (subtle HF noise on DSD playback)
**Symptom:** Faint "chhhh" white noise audible in tweeters during DSD playback, especially on quiet solo piano passages
**Fix:** Byte-swap in wrapper de-interleave loop

---

## 1. Symptom

When playing PGGB-upsampled DSD512 files through squeeze2diretta, a subtle broadband high-frequency noise ("chhhh") was audible in the tweeters during quiet passages (solo piano). The same files played through DirettaRendererUPnP-X exhibited no such noise.

**Test equipment:** Holo Audio DAC (direct DSD path) + Benchmark AHB2 amplifier — a reference-grade chain transparent enough to reveal the artifact.

---

## 2. Root Cause

A byte-order mismatch in the DSD data path between Squeezelite's stdout output and the wrapper's de-interleaving.

### 2.1 How Squeezelite packs DSD for stdout

In `dsd.c`, `_decode_dsf()` packs 4 DSF input bytes into a `uint32_t` (lines 327-329):

```c
*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 24 | dsd2pcm_bitreverse[*(iptrl+1)] << 16
          | dsd2pcm_bitreverse[*(iptrl+2)] << 8  | dsd2pcm_bitreverse[*(iptrl+3)];
```

Input byte[0] is shifted to bits 31-24 (MSB of the uint32_t). On a **little-endian** machine (x86, most ARM), this uint32_t is stored in memory as:

```
memory[0] = byte[3] (LSB — last DSD byte)
memory[1] = byte[2]
memory[2] = byte[1]
memory[3] = byte[0] (MSB — first DSD byte)
```

### 2.2 How stdout writes it

In `output_stdout.c`, the output format is hardcoded to `S32_LE` (line 141):

```c
output.format = S32_LE;
```

When `_scale_and_pack_frames()` in `output_pack.c` handles `S32_LE` on a LE machine (lines 322-324):

```c
case S32_LE:
    if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
        memcpy(outputptr, inputptr, cnt * BYTES_PER_FRAME);
```

It does a plain **memcpy** — preserving the LE memory layout. The bytes arrive on the pipe in LE order: `[byte3, byte2, byte1, byte0]`.

### 2.3 The missed byte-swap

`output_pack.c` has a `U32_BE` case (lines 93-106) that correctly byte-swaps on LE machines:

```c
case U32_BE:
#if SL_LITTLE_ENDIAN
    *(optr++) = (lsample & 0xff000000) >> 24 | (lsample & 0x00ff0000) >> 8 |
                (lsample & 0x0000ff00) << 8  | (lsample & 0x000000ff) << 24;
```

But this code path is **never reached** for stdout output because `output.format` is always `S32_LE`, regardless of the DSD format selected with `-D :u32be`.

### 2.4 The wrapper's assumption

The wrapper copied bytes as-is during de-interleaving:

```cpp
planar_buffer[dst_offset_L + 0] = buffer[src_offset + 0];  // = byte3 (LAST DSD byte)
planar_buffer[dst_offset_L + 1] = buffer[src_offset + 1];  // = byte2
planar_buffer[dst_offset_L + 2] = buffer[src_offset + 2];  // = byte1
planar_buffer[dst_offset_L + 3] = buffer[src_offset + 3];  // = byte0 (FIRST DSD byte)
```

The result: every 4-byte DSD group had its temporal byte order **reversed**.

---

## 3. Why It Sounds Like White Noise (Not Distortion)

Each 4-byte group contains 32 consecutive DSD samples. Reversing the byte order within each group disrupts the 1-bit stream's temporal coherence at the 32-sample boundary, but preserves the statistical properties over longer time scales.

At DSD512 (22.5 MHz), the disruption occurs every 32 / 22,579,200 = ~1.4 microseconds. The resulting noise energy is concentrated at ~700 kHz and harmonics — ultrasonic, but:

- The DAC's direct DSD path passes it with minimal filtering
- The amplifier (200kHz bandwidth) faithfully amplifies it
- Intermodulation products fold down into the audible range
- The tweeters partially reproduce it directly

The audio content is largely preserved because DSD's low-frequency information is carried by long-range bit correlations that survive the intra-group byte reversal. This is why the music sounds correct but has a subtle noise floor elevation — not distortion.

---

## 4. Why DirettaRendererUPnP-X Is Not Affected

In the -X path, DSD data comes from FFmpeg's AudioEngine, which provides raw DSD bytes directly — no uint32_t packing, no LE/BE memory layout issue. The DirettaSync and ring buffer code is shared between both projects, confirming the issue is isolated to the Squeezelite stdout interface.

---

## 5. The Fix

Byte-swap during de-interleaving in `squeeze2diretta-wrapper.cpp` (both main loop and burst-fill loop):

```cpp
// Before (wrong — preserves LE pipe order):
planar_buffer[dst_offset_L + 0] = buffer[src_offset + 0];
planar_buffer[dst_offset_L + 1] = buffer[src_offset + 1];
planar_buffer[dst_offset_L + 2] = buffer[src_offset + 2];
planar_buffer[dst_offset_L + 3] = buffer[src_offset + 3];

// After (correct — restores temporal DSD byte order):
planar_buffer[dst_offset_L + 0] = buffer[src_offset + 3];
planar_buffer[dst_offset_L + 1] = buffer[src_offset + 2];
planar_buffer[dst_offset_L + 2] = buffer[src_offset + 1];
planar_buffer[dst_offset_L + 3] = buffer[src_offset + 0];
```

---

## 6. Files Modified

| File | Change |
|------|--------|
| `squeeze2diretta-wrapper.cpp` | Byte-swap in main DSD de-interleave loop (~line 1063) |
| `squeeze2diretta-wrapper.cpp` | Byte-swap in burst-fill DSD de-interleave loop (~line 857) |
| `squeeze2diretta-wrapper.cpp` | Updated comment at DSD format declaration (~line 729) |

---

## 7. Verification

**Before fix:** Subtle "chhhh" audible in tweeters during quiet DSD passages.
**After fix:** Silent background, no high-frequency noise artifact.

Tested with PGGB DSD512 solo piano recordings on Holo Audio (direct DSD) + Benchmark AHB2.
