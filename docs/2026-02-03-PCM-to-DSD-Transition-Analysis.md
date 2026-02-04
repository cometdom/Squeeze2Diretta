# High-Profile Engineering Analysis: Format Change Handling

**Date:** 2026-02-03
**Scope:** Squeeze2Diretta vs DirettaRendererUPnP-X
**Problem:** PCM 352.8kHz (8fs) → DSD512 transition: DAC acknowledges format change, but music stops playing

---

## 1. Corrected Problem Statement

**Original assumption (INCORRECT):** The race condition in asynchronous detection causes DSD512→PCM failures.

**Actual problem (CORRECT):** PCM8fs → DSD512 transitions fail **despite successful detection**. The DAC acknowledges the format change, indicating:
- Detection works (stderr monitor sees the change)
- DirettaSync::open() succeeds (format is configured correctly)
- setSink() succeeds (DAC confirms new format)
- Connection is established (target goes online)

**But music stops playing** - this points to a **post-transition data flow issue**, not a detection issue.

---

## 2. Root Cause Analysis

### 2.1 Transition Path Trace (PCM 352.8kHz → DSD512)

```
Timeline:
────────────────────────────────────────────────────────────────────────
T0: stderr monitor detects "format: DSD_U32_BE" + "sample rate: 705600"
T1: g_need_reopen.store(true)
T2: Main loop sees g_need_reopen=true (wrapper.cpp:571)
    │
    ├─ isExtremeTransition check (line 621-624):
    │    (!format.isDSD && format.sampleRate >= 176400 && is_dsd)
    │    = (!false && 352800 >= 176400 && true) = TRUE ✓
    │
    ├─ Sleep 300ms (line 630)
    │
    ├─ Drain up to 256KB (lines 634-670)
    │
    ├─ Update format: isDSD=true, sampleRate=22,579,200 (lines 675-678)
    │
    └─ Call g_diretta->open(format) (line 701)
        │
        ├─ DirettaSync detects format change:
        │    wasDSD=false, nowDSD=true → PCM→DSD path
        │
        ├─ Calls reopenForFormatChange() (line 578-582)
        │    ├─ stop()
        │    ├─ disconnect(true)
        │    ├─ Worker thread join
        │    ├─ DIRETTA::Sync::close()
        │    ├─ Wait 800ms (m_config.formatSwitchDelayMs)
        │    └─ DIRETTA::Sync::open() fresh
        │
        ├─ fullReset() - sets m_prefillComplete=false, etc.
        │
        ├─ configureSinkDSD() + configureRingDSD()
        │
        ├─ Wait 500ms (initialDelayMs)
        │
        ├─ setSink() → DAC acknowledges format ✓
        │
        ├─ connectPrepare() + connect() + connectWait() → Online ✓
        │
        └─ startSyncWorker() → Worker thread starts

T3: open() returns successfully
    │
    ├─ Send 3 silence buffers (12KB total) (lines 717-733)
    │
    ├─ Wait 20ms (line 736)
    │
    └─ Resume main read loop (line 759)

T4: Worker thread calls getNewStream()
    │
    ├─ Check m_prefillComplete → FALSE
    │
    └─ Return SILENCE (0x69) to DAC indefinitely
────────────────────────────────────────────────────────────────────────
```

### 2.2 The Critical Bug: Prefill vs Silence Sent

**DirettaSync.cpp prefill logic** (getNewStream):
```cpp
if (!m_prefillComplete.load(std::memory_order_acquire)) {
    std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
    return true;  // ← DAC receives silence, not actual audio
}
```

**Prefill target for DSD512:**
- Ring buffer typically sized for ~0.8 seconds
- At DSD512 stereo: 22,579,200 bits/s × 2 ch = 5.6 MB/s
- Prefill target ≈ 0.8s × 5.6 MB/s = **~4.5 MB**

**Silence sent by wrapper after open():**
```cpp
const size_t SILENCE_FRAMES = 512;  // ~10ms at 48kHz
size_t silence_bytes = SILENCE_FRAMES * new_bytes_per_frame;  // 512 × 8 = 4096 bytes
int num_buffers = 3;  // For DSD
// Total: 3 × 4096 = 12,288 bytes ≈ 12 KB
```

**The Gap:**
| Metric | Value |
|--------|-------|
| Prefill target | ~4.5 MB (estimated) |
| Silence sent | 12 KB |
| Coverage | **0.27%** |

The wrapper sends only **0.27%** of the prefill requirement before expecting audio to play!

### 2.3 Why This Causes Audio Dropout

```
Sequence after open() returns:
═══════════════════════════════════════════════════════════════════════
Ring Buffer State:        [▓▓░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]
                           12KB                                   4.5MB
                           └── Silence sent by wrapper

Worker Thread:            getNewStream() → prefillComplete=FALSE
                          → Returns SILENCE to DAC
                          → DAC plays: silence, silence, silence...

Wrapper Main Loop:        read() → sendAudio() → pushes data to ring
                          Rate-limited to match sample rate!

Time to fill prefill at rate-limited pace:
  - DSD512: 705,600 frames/s × 8 bytes/frame = 5.6 MB/s
  - Prefill: 4.5 MB / 5.6 MB/s = ~0.8 seconds

BUT: Worker consumes at the SAME rate (pull model)
  - Push rate ≈ Pull rate
  - Ring buffer level stays ~constant (not increasing)
  - Prefill NEVER reached!
═══════════════════════════════════════════════════════════════════════
```

**Root Cause:** The wrapper rate-limits audio push to match the sample rate. But the worker thread pulls at the same rate. Since prefill wasn't met initially (only 12KB vs 4.5MB needed), and push rate equals pull rate, **the ring buffer level never increases enough to reach prefill**.

### 2.4 Comparison with DirettaRendererUPnP-X

In DirettaRendererUPnP-X, format changes are **synchronous** within the audio callback:

```cpp
// DirettaRenderer.cpp audio callback
void audioCallback(const uint8_t* data, size_t samples, ...) {
    AudioFormat format = buildFormatFromTrackInfo();

    if (formatChanged(format)) {
        // Detection happens HERE, with audio data IN HAND
        m_direttaSync->open(format);  // Configure new format
    }

    // Immediately send the audio we have (which is already new format)
    m_direttaSync->sendAudio(data, samples);
}
```

**Key differences:**
1. Format change detected AT the moment of data arrival
2. The audio data in the callback IS the new format (no mixed data)
3. First audio frame of new format is immediately available
4. Prefill can start with real audio, not just a tiny silence buffer

---

## 3. Conceptual Understanding of Differences

### 3.1 Synchronous vs Asynchronous: The Deeper Issue

The problem isn't just "async detection is late" - it's that:

| Aspect | DirettaRendererUPnP-X | Squeeze2Diretta |
|--------|----------------------|-----------------|
| **First audio after transition** | Real audio data (from callback) | 12KB silence only |
| **Prefill strategy** | Starts with actual content | Starts near-empty |
| **Rate balance** | Producer has burst of data ready | Producer rate-limited from T0 |
| **Time to stable playback** | Immediate (data already in hand) | Never (equilibrium trap) |

### 3.2 The Equilibrium Trap

In Squeeze2Diretta:
```
After transition:
  Ring buffer level = 12KB (silence)

Each cycle:
  Push: 1 buffer worth of data
  Pull: 1 buffer worth of data
  Net change: ~0

Prefill threshold: 4.5MB
Current level: 12KB + (push - pull) ≈ 12KB
Status: NEVER reaches prefill
```

In DirettaRendererUPnP-X:
```
At transition moment:
  Audio callback provides: 4096 samples of NEW format
  Ring buffer receives: Real audio immediately

First few cycles:
  Callback keeps providing data (AudioEngine has decoded ahead)
  Ring buffer fills rapidly
  Prefill reached within 10-50ms
```

### 3.3 Why PCM→DSD is Worse Than DSD→PCM

For **DSD→PCM** transitions:
- Previous format was DSD (high byte rate)
- Ring buffer was already filled with DSD data
- After transition, some old DSD data is drained but buffer isn't empty
- PCM rate is typically lower, so prefill target is smaller
- Easier to meet prefill with residual data + new audio

For **PCM→DSD** transitions (especially high-rate PCM → DSD512):
- Previous format was PCM (even at 352.8kHz, ~2.8 MB/s)
- Ring buffer is aggressively drained (256KB removed!)
- After transition, buffer is nearly empty
- DSD512 has HIGHEST byte rate (5.6 MB/s)
- Largest prefill target, smallest initial content
- Maximum difficulty to escape equilibrium trap

---

## 4. Methodological Proposal

### 4.1 Core Insight

The fix must address the **equilibrium trap** by ensuring the ring buffer reaches prefill before steady-state pull begins.

**Two approaches:**

| Approach | Description | Risk |
|----------|-------------|------|
| **A: Burst Fill** | Disable rate-limiting until prefill is reached | Low |
| **B: Reduce Prefill** | Lower prefill target for transitions | Medium |
| **C: Preload Silence** | Fill ring buffer with silence before starting worker | Low |

### 4.2 Pattern Application (from Optimisation Methodology)

**Pattern #3: Decision Point Relocation**
Move the rate-limiting decision from "always limit" to "limit only after prefill".

**Pattern #7: Flow Control Tuning**
Adaptive flow control based on buffer state - aggressive push when underfilled.

### 4.3 Recommended Solution: Burst Fill + Reduced Initial Prefill

```cpp
// In wrapper, after open() returns:

// NEW: Burst-fill until DirettaSync signals prefill complete
const size_t BURST_CHUNK = 8192;  // 8KB chunks
std::vector<uint8_t> burst_buffer(BURST_CHUNK);

while (!g_diretta->isPrefillComplete() && running) {
    ssize_t bytes_read = read(fifo_fd, burst_buffer.data(), BURST_CHUNK);
    if (bytes_read > 0) {
        // Process and send WITHOUT rate limiting
        processAndSend(burst_buffer.data(), bytes_read, format);
    } else if (bytes_read == 0) {
        // Pipe empty - send silence to continue filling
        std::fill(burst_buffer.begin(), burst_buffer.end(),
                  format.isDSD ? 0x69 : 0x00);
        processAndSend(burst_buffer.data(), BURST_CHUNK, format);
    }
}

// NOW enable rate limiting for steady-state
start_time = std::chrono::steady_clock::now();
frames_sent = 0;
```

---

## 5. Practical Implementation Plan

### Phase 1: Diagnostic Verification (1 hour)

Before implementing fixes, verify the hypothesis:

**Task 1.1: Add prefill logging**

In `DirettaSync.cpp`, add logging to `getNewStream()`:

```cpp
// After line ~1451 (prefill check)
if (!m_prefillComplete.load(std::memory_order_acquire)) {
    size_t avail = m_ringBuffer.getAvailable();
    static int logCount = 0;
    if (logCount++ % 100 == 0) {  // Log every 100th call
        std::cout << "[Prefill] Waiting: " << avail << "/" << m_prefillTarget
                  << " bytes (" << (avail * 100 / m_prefillTarget) << "%)" << std::endl;
    }
    std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
    return true;
}
```

**Task 1.2: Add ring buffer level to transition logging**

In `wrapper.cpp`, after silence is sent (line ~737):

```cpp
std::cout << "[Post-Transition] Ring buffer: " << g_diretta->getBufferLevel() * 100
          << "%, Prefill target: " << g_diretta->getPrefillTarget() << " bytes" << std::endl;
```

**Expected output if hypothesis is correct:**
```
[Post-Transition] Ring buffer: 0.3%, Prefill target: 4500000 bytes
[Prefill] Waiting: 12288/4500000 bytes (0%)
[Prefill] Waiting: 16384/4500000 bytes (0%)
... (prefill never reached)
```

### Phase 2: Immediate Fix - Burst Fill (2 hours)

**File:** `squeeze2diretta-wrapper.cpp`

**Task 2.1: Add isPrefillComplete() accessor to DirettaSync**

In `DirettaSync.h`:
```cpp
public:
    bool isPrefillComplete() const {
        return m_prefillComplete.load(std::memory_order_acquire);
    }
    size_t getPrefillTarget() const { return m_prefillTarget; }
```

**Task 2.2: Implement burst-fill loop after format change**

Replace lines 715-757 with:

```cpp
// After open() succeeds (line 705):

// Calculate new bytes per frame
size_t new_bytes_per_frame;
if (is_dsd) {
    new_bytes_per_frame = 4 * format.channels;
} else {
    new_bytes_per_frame = (format.bitDepth / 8) * format.channels;
}

bytes_per_frame = new_bytes_per_frame;
buffer_size = CHUNK_SIZE * bytes_per_frame;
buffer.resize(buffer_size);

// CRITICAL: Burst-fill until prefill is complete
// This escapes the equilibrium trap where push rate = pull rate
std::cout << "[Burst Fill] Starting prefill burst..." << std::endl;

auto burst_start = std::chrono::steady_clock::now();
const auto burst_timeout = std::chrono::seconds(3);  // Safety timeout

while (!g_diretta->isPrefillComplete() && running) {
    // Check timeout
    if (std::chrono::steady_clock::now() - burst_start > burst_timeout) {
        std::cerr << "[Burst Fill] WARNING: Prefill timeout after 3s" << std::endl;
        break;
    }

    // Try to read data (non-blocking would be better, but use short timeout)
    struct pollfd pfd = {fifo_fd, POLLIN, 0};
    int ret = poll(&pfd, 1, 10);  // 10ms timeout

    if (ret > 0 && (pfd.revents & POLLIN)) {
        ssize_t bytes_read = read(fifo_fd, buffer.data(), buffer_size);
        if (bytes_read > 0) {
            // Process normally but without rate limiting
            if (is_dsd && dsd_format != DSDFormatType::DOP) {
                // De-interleave native DSD
                // ... (existing DSD processing code)
            } else if (dsd_format == DSDFormatType::DOP) {
                // DoP conversion
                // ... (existing DoP processing code)
            } else {
                // PCM direct send
                g_diretta->sendAudio(buffer.data(), bytes_read / bytes_per_frame);
            }
        }
    } else {
        // No data available - send silence to help reach prefill
        std::vector<uint8_t> silence(buffer_size, format.isDSD ? 0x69 : 0x00);
        size_t silence_samples = format.isDSD ?
            (buffer_size * 8) / format.channels : CHUNK_SIZE;
        g_diretta->sendAudio(silence.data(), silence_samples);
    }
}

std::cout << "[Burst Fill] Prefill complete, switching to rate-limited mode" << std::endl;

// Reset timing for rate-limited steady-state
start_time = std::chrono::steady_clock::now();
frames_sent = 0;

std::cout << "[Diretta Reopened] Ready for " << (format.isDSD ? "DSD" : "PCM")
          << " at " << format.sampleRate << "Hz" << std::endl;
```

### Phase 3: Alternative/Complementary Fix - Reduced Transition Prefill (1 hour)

**File:** `DirettaSync.cpp`

**Task 3.1: Add reduced prefill for format transitions**

In `fullReset()` (around line 843), add a flag:

```cpp
void DirettaSync::fullReset() {
    // ... existing code ...

    // Use reduced prefill for format transitions
    // Full prefill will be restored after first audio plays
    m_useReducedPrefill = true;
}
```

In `configureRingDSD()`, apply reduced prefill:

```cpp
void DirettaSync::configureRingDSD(uint32_t byteRate, int channels) {
    // ... existing calculation ...

    if (m_useReducedPrefill) {
        // 10% of normal prefill for transitions
        m_prefillTarget = m_prefillTarget / 10;
        std::cout << "[DirettaSync] Using reduced prefill for transition: "
                  << m_prefillTarget << " bytes" << std::endl;
        m_useReducedPrefill = false;  // Reset for next time
    }
}
```

### Phase 4: Verify with Testing Checklist

| Test | Expected Result |
|------|-----------------|
| PCM 44.1kHz → DSD64 | Audio plays within 500ms |
| PCM 352.8kHz → DSD512 | Audio plays within 1s |
| DSD512 → PCM 352.8kHz | Audio plays within 500ms |
| DSD64 → DSD512 | Audio plays within 500ms |
| Rapid format switching | No hangs, no crashes |

---

## 6. Summary

### Root Cause (Corrected)

The PCM→DSD transition failure is NOT due to asynchronous detection but due to **an equilibrium trap** in the post-transition data flow:

1. Wrapper sends only 12KB silence after `open()` returns
2. Prefill target for DSD512 is ~4.5MB
3. Wrapper rate-limits at sample rate
4. Worker pulls at sample rate
5. Push ≈ Pull → Ring buffer never fills → Prefill never reached → Silence forever

### Solution Priority

| Priority | Fix | Impact | Effort |
|----------|-----|--------|--------|
| **1** | Diagnostic logging | Verify hypothesis | 30 min |
| **2** | Burst-fill after transition | Escape equilibrium trap | 2 hours |
| **3** | Reduced transition prefill | Faster recovery | 1 hour |

### Key Insight

DirettaRendererUPnP-X doesn't have this problem because the AudioEngine provides a **burst of decoded audio** at the moment of format change. The synchronous callback model ensures the first audio of the new format is immediately available, allowing prefill to complete before steady-state pull begins.

Squeeze2Diretta's asynchronous model means the wrapper must **artificially create this burst** by disabling rate-limiting until prefill is achieved.

---

## 7. Implementation (2026-02-03)

The following changes were implemented to fix the PCM→DSD transition issue:

### 7.1 Files Modified

| File | Changes |
|------|---------|
| `diretta/DirettaSync.h` | Added `isPrefillComplete()` and `getPrefillTarget()` accessor methods |
| `diretta/DirettaSync.cpp` | Added diagnostic logging to prefill check in `getNewStream()` |
| `squeeze2diretta-wrapper.cpp` | Replaced simple silence-send with burst-fill loop |

### 7.2 DirettaSync.h Changes

Added public accessors for prefill state:

```cpp
/**
 * @brief Check if prefill is complete (ring buffer has enough data to start playback)
 * @return true if prefill threshold has been reached
 *
 * Used by wrapper to implement burst-fill after format transitions.
 * Without burst-fill, push rate equals pull rate, causing equilibrium trap
 * where prefill is never reached.
 */
bool isPrefillComplete() const {
    return m_prefillComplete.load(std::memory_order_acquire);
}

/**
 * @brief Get the prefill target in bytes
 * @return Number of bytes needed in ring buffer before playback starts
 */
size_t getPrefillTarget() const { return m_prefillTarget; }
```

### 7.3 DirettaSync.cpp Changes

Added diagnostic logging to `getNewStream()` prefill check (around line 1451):

```cpp
// Prefill not complete
if (!m_prefillComplete.load(std::memory_order_acquire)) {
    // Diagnostic: Log prefill progress periodically to debug equilibrium trap
    static int prefillLogCount = 0;
    size_t avail = m_ringBuffer.getAvailable();
    if (prefillLogCount++ % 50 == 0) {  // Log every 50th call (~100ms at typical rates)
        float pct = (m_prefillTarget > 0) ? (100.0f * avail / m_prefillTarget) : 0.0f;
        std::cout << "[Prefill] Waiting: " << avail << "/" << m_prefillTarget
                  << " bytes (" << pct << "%)"
                  << (currentIsDsd ? " [DSD]" : " [PCM]") << std::endl;
    }
    std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
    m_workerActive = false;
    return true;
}
```

### 7.4 Wrapper Burst-Fill Implementation

Replaced the simple 12KB silence-send with a comprehensive burst-fill loop that:

1. **Pushes data without rate-limiting** until prefill is complete
2. **Handles all DSD formats** (DoP, U32_BE, U32_LE) with proper conversion
3. **Falls back to silence** if pipe is temporarily empty
4. **Has a 5-second timeout** as safety measure
5. **Logs progress** periodically for debugging

Key aspects of the burst-fill loop:

```cpp
while (!g_diretta->isPrefillComplete() && running) {
    // Try to read data (with short timeout to stay responsive)
    struct pollfd pfd = {fifo_fd, POLLIN, 0};
    int ret = poll(&pfd, 1, 50);  // 50ms timeout

    if (ret > 0 && (pfd.revents & POLLIN)) {
        // Read and process data (DSD conversion if needed)
        // Send WITHOUT rate limiting
    } else {
        // No data available - send silence to help reach prefill
        // This prevents deadlock if pipe is temporarily empty
    }
}

// After prefill complete, reset timing for rate-limited steady-state
start_time = std::chrono::steady_clock::now();
frames_sent = 0;
```

### 7.5 Expected Behavior After Fix

**Before fix:**
```
[Diretta Reopened] Ready for DSD at 22579200Hz
[Prefill] Waiting: 12288/4500000 bytes (0.3%) [DSD]
[Prefill] Waiting: 16384/4500000 bytes (0.4%) [DSD]
... (prefill never reached, silence forever)
```

**After fix:**
```
[Burst Fill] Starting prefill for DSD at 22579200Hz...
[Burst Fill] Prefill target: 4500000 bytes
[Burst Fill] Progress: 25.0%, sent 1125000 bytes (silence fills: 0)
[Burst Fill] Progress: 50.0%, sent 2250000 bytes (silence fills: 0)
[Burst Fill] Progress: 75.0%, sent 3375000 bytes (silence fills: 0)
[Burst Fill] Complete: 4500000 bytes in 200ms (80 iterations, 0 silence fills)
[Diretta Reopened] Ready for DSD at 22579200Hz
... (audio plays normally)
```

### 7.6 Testing Checklist

| Test | Expected Result | Status |
|------|-----------------|--------|
| PCM 44.1kHz → DSD64 | Audio plays within 500ms | Pending |
| PCM 352.8kHz → DSD512 | Audio plays within 1s | Pending |
| DSD512 → PCM 352.8kHz | Audio plays within 500ms | Pending |
| DSD64 → DSD512 | Audio plays within 500ms | Pending |
| Rapid format switching | No hangs, no crashes | Pending |
| Burst-fill timeout | Warning logged, graceful fallback | Pending |

---

## 8. Same-Family High-Rate Transition Fix (2026-02-03)

### 8.1 Problem Description

After implementing the burst-fill fix, transitions within the 48kHz clock family worked correctly. However, transitions within the 44.1kHz family still failed:

| Transition | Clock Family | Result |
|------------|--------------|--------|
| PCM 384kHz → DSD512×48 | 48kHz | ✓ Works |
| DSD512×48 → PCM 384kHz | 48kHz | ✓ Works |
| PCM 352.8kHz → DSD512×44.1 | 44.1kHz | ✗ No sound |
| DSD512×44.1 → PCM 352.8kHz | 44.1kHz | ✓ Works (already uses full reset) |
| Cross-family transitions | Mixed | ✓ Works (naturally resets PLL) |

**Key observation:** Going through 44.1kHz base rate (or switching to a 48kHz family format) always restored sound.

### 8.2 Root Cause Analysis

**Clock families:**
- **44.1kHz family:** 44100, 88200, 176400, 352800 Hz (PCM) and DSD rates divisible by 44100
- **48kHz family:** 48000, 96000, 192000, 384000, 768000 Hz (PCM) and DSD rates divisible by 48000

The Diretta target's PLL (Phase-Locked Loop) gets "stuck" when transitioning between high-rate formats within the same clock family. The `reopenForFormatChange()` method does not fully reset the PLL in this case, whereas:

1. **Cross-family transitions** naturally reset the PLL (different master clock)
2. **Base-rate transitions** (through 44.1kHz or 48kHz) reset the PLL
3. **Full close/reopen** completely resets all target state including PLL

### 8.3 Code Path Analysis

Before fix, PCM→DSD transitions always used `reopenForFormatChange()`:

```
DirettaSync::open() format change detection:
│
├─ wasDSD && (nowPCM || isDsdRateChange) → Full close/reopen ✓
│
├─ isPcmRateChange → Full close/reopen ✓
│
└─ else (PCM→DSD) → reopenForFormatChange() ✗ (insufficient for same-family high-rate)
```

### 8.4 Fix Implementation

Added clock family detection to the PCM→DSD path in `DirettaSync.cpp`:

```cpp
} else {
    // PCM→DSD (or bit depth change)
    // Detect same-family high-rate transitions that need full reset

    // Helper lambda to determine clock family (441 = 44.1kHz, 480 = 48kHz)
    auto getClockFamily = [](uint32_t sampleRate) -> int {
        if (sampleRate % 44100 == 0) return 441;
        if (sampleRate % 48000 == 0) return 480;
        return 0;
    };

    int oldFamily = getClockFamily(m_previousFormat.sampleRate);
    int newFamily = getClockFamily(format.sampleRate);
    bool sameFamily = (oldFamily != 0 && oldFamily == newFamily);

    // High-rate: PCM 176.4kHz+ (4fs) or DSD256+ (which corresponds to 4fs base)
    bool oldIsHighRate = m_previousFormat.sampleRate >= 176400;
    bool newIsHighRate = format.sampleRate >= 11289600;  // DSD256×44.1 = 11,289,600

    bool needsFullReset = sameFamily && (oldIsHighRate || newIsHighRate);

    if (needsFullReset) {
        // Same clock family high-rate PCM→DSD transition
        // Full close/reopen required to reset target's PLL
        // ... (full close/reopen sequence)
    } else {
        // Different clock family or low-rate: reopenForFormatChange() is sufficient
        if (!reopenForFormatChange()) { ... }
    }
}
```

### 8.5 Decision Logic Summary

| Previous Format | New Format | Same Family | High Rate | Action |
|-----------------|------------|-------------|-----------|--------|
| PCM 44.1kHz | DSD64×44.1 | Yes | No | `reopenForFormatChange()` |
| PCM 352.8kHz | DSD512×44.1 | Yes | Yes | Full close/reopen |
| PCM 384kHz | DSD512×48 | Yes | Yes | Full close/reopen |
| PCM 352.8kHz | DSD512×48 | No | Yes | `reopenForFormatChange()` |
| PCM 48kHz | DSD64×44.1 | No | No | `reopenForFormatChange()` |

### 8.6 Expected Log Output

**Before fix (44.1kHz family, high-rate):**
```
[DirettaSync] Format change - reopen
[DirettaSync] ... (uses reopenForFormatChange)
... (no sound)
```

**After fix (44.1kHz family, high-rate):**
```
[DirettaSync] High-rate PCM->DSD512 (same 441Hz family) - full close/reopen
[DirettaSync] Waiting 1600ms for target to reset...
[DirettaSync] DIRETTA::Sync reopened
... (sound plays)
```

### 8.7 Updated Testing Checklist

| Test | Clock Family | Expected Result | Status |
|------|--------------|-----------------|--------|
| PCM 44.1kHz → DSD64×44.1 | 44.1kHz (low-rate) | Uses reopenForFormatChange | Pending |
| PCM 352.8kHz → DSD512×44.1 | 44.1kHz (high-rate) | Full close/reopen, sound plays | Pending |
| PCM 384kHz → DSD512×48 | 48kHz (high-rate) | Full close/reopen, sound plays | Pending |
| PCM 352.8kHz → DSD512×48 | Cross-family | Uses reopenForFormatChange, sound plays | Pending |
| DSD512×44.1 → PCM 352.8kHz | 44.1kHz | Full close/reopen (existing code) | Pending |
