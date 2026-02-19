# Changelog

All notable changes to squeeze2diretta will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.2] - 2026-02-19

### Added

**Idle Target Release (coexistence with DirettaRendererUPnP):**
- When both services run simultaneously, squeeze2diretta now releases the Diretta Target after 5 seconds of inactivity
- Previously, the SDK connection was held indefinitely, preventing DirettaRendererUPnP from connecting
- Uses `poll()` on the squeezelite pipe to detect idle state, then calls `release()` to free the target
- Target is automatically re-acquired when LMS starts the next track (~500ms overhead on first track after idle)
- Logged as `No activity for 5s — releasing Diretta target for other sources`

**Rebuffering on Underrun (streaming resilience):**
- When the ring buffer empties during a network stall, small data bursts were immediately consumed, creating a rapid silence/audio alternation ("CD skip" effect)
- Now enters rebuffering mode on underrun: holds silence until the buffer refills to 20%
- Result: clean silence gap followed by smooth playback resumption instead of stuttering
- Logged as `Buffer underrun — entering rebuffering mode` / `Rebuffering complete — resuming playback`

---

## [2.0.1] - 2026-02-17

### Added

**Centralized Log Level System:**
- New `LogLevel.h` header with 4 levels: ERROR, WARN, INFO, DEBUG
- `-q` / `--quiet` option: show only warnings and errors (WARN level)
- `-v` continues to work as before (DEBUG level)
- Default level (INFO) produces the same output as v2.0.0
- All source files migrated from per-file `DEBUG_LOG`/`DIRETTA_LOG` macros to unified log levels
- In `NOLOG` builds, all logging macros compile to no-ops

**Runtime Statistics via SIGUSR1:**
- Send `kill -USR1 <pid>` to dump live statistics to stdout
- Shows: playback state, current format, buffer fill level, MTU, stream/push/underrun counters
- Useful for monitoring production systems via systemd journal

**MS Mode Negotiation Logging:**
- Verbose log now shows the MS mode negotiated with the Diretta Target
- From second track onwards: supported modes, requested mode, and negotiated mode
- First track: clear message that MS info becomes available after first connection
- Uses "negotiated" wording to clarify the mode is inferred from AUTO algorithm + target capabilities

### Fixed

**Squeezelite 100% CPU When Idle:**
- The output thread in `output_stdout.c` was spinning without sleep when no audio data was available
- Added `usleep(10000)` in the two idle paths (before first track, and between tracks)
- CPU usage drops from 100% to ~0% when idle, with no audible impact during playback
- Requires rebuilding squeezelite with the updated `squeezelite-format-header.patch`

**Atomic Ordering Fix in RingAccessGuard:**
- Changed `fetch_add` from `memory_order_acquire` to `memory_order_acq_rel`
- Ensures the increment is visible to the reconfiguration thread on all architectures (ARM64)

### Added (ARM Architecture)

**ARM NEON SIMD Format Conversions:**
- Hand-optimized NEON intrinsics for all PCM and DSD format conversions on ARM64
- PCM: `convert24BitPacked` (LSB/MSB), `convert16To32` using `vzip`/`vshrn`/`vmovn` intrinsics
- DSD: all 4 conversion modes (Passthrough, BitReverse, ByteSwap, BitReverseSwap) using `vzip1q_u32`/`vzip2q_u32` interleaving
- Bit reversal via `vqtbl1q_u8` LUT-based nibble swap, byte swap via `vrev32q_u8`
- Automatic detection via `DIRETTA_HAS_NEON` macro (`__aarch64__` + `__ARM_NEON`)
- Fallback to scalar code when NEON is not available
- Ported from DirettaRendererUPnP v2.0.4

### Changed

**Production Build in install.sh:**
- `install.sh` now configures CMake with `-DNOLOG=ON` by default (disables SDK internal logging)
- Application-level logging (`-v`/`-q`) remains fully functional
- New `NOLOG` CMake option added to `CMakeLists.txt`

**Updated systemd Configuration:**
- `squeeze2diretta.conf`: documented `-q` option alongside `-v`
- `start-squeeze2diretta.sh`: passes `$VERBOSE` value directly instead of hardcoded `-v`

---

## [2.0.0] - 2026-02-14

### UPGRADE NOTICE
v2.0 requires a **new patched squeezelite** with in-band format headers.
You MUST rebuild squeezelite using the updated `setup-squeezelite.sh`.
The old `squeezelite-stdout-flush.patch` is replaced by `squeezelite-format-header.patch`.

```bash
# Remove old squeezelite and rebuild
sudo systemctl stop squeeze2diretta
rm -rf squeezelite/
./setup-squeezelite.sh
```

### Changed
- **In-band format signaling (breaking change)**: squeezelite now writes a 16-byte
  binary header ("SQFH") to stdout at each track boundary, containing sample rate,
  bit depth, DSD format, and channel count. The wrapper reads this header synchronously,
  eliminating the stderr log parsing race condition that caused most v1.x bugs.
- **Consumer-driven flow control**: replaced `sleep_until()` rate limiting with
  event-based backpressure using DirettaSync's `waitForSpace()` / `notifySpaceAvailable()`
  mechanism. The Diretta SDK's actual consumption rate drives data delivery, reducing
  timing jitter from ±2ms to ±50µs for smoother audio streaming.
- **Removed stderr monitoring**: the wrapper no longer parses squeezelite's stderr
  logs for format detection. Squeezelite's stderr now passes through to the parent
  process for debugging visibility.
- **Simplified process setup**: only one pipe (stdout) instead of two (stdout + stderr).
  No more stderr monitor thread, atomic globals, drain logic, or extreme transition delays.
- **Gapless playback**: same-format track transitions are detected via header comparison
  and handled without reopening Diretta (quick resume).
- **Buffered pipe reader**: new `PipeReader` class with peek support for reliable
  header detection mid-stream.

### Fixed
- **Format transitions (DSD↔PCM)** now use full SDK close/reopen with fresh
  `openSyncConnection()` for clean state, fixing noise and wrong-speed playback
  on repeated transitions.
- **Missed format changes**: `PipeReader::readUpTo()` now scans for embedded SQFH
  headers within pipe data, preventing them from being consumed as audio data.
  This was the root cause of the DAC staying locked at the first track's format.

### Removed
- `monitor_squeezelite_stderr()` function and thread
- `g_need_reopen`, `g_format_pending`, `g_is_dsd`, `g_dsd_format_type`, `g_current_sample_rate` atomics
- Stderr pipe creation and redirect
- Pre-format data drain logic
- `sleep_until()` time-based rate limiting (replaced by consumer-driven flow control)
- Extreme transition delays (PCM→DSD: 50ms, DSD→PCM: 80ms)
- Initial "wait for format" spin-loop (replaced by blocking header read)
- `squeezelite-stdout-flush.patch` (replaced by `squeezelite-format-header.patch`)

---

## [1.0.2] - 2026-02-09

### Fixed
- **24-bit PCM noise/distortion when LMS digital volume is not 100%**
  - Root cause: squeezelite outputs MSB-aligned S32_LE (24-bit data left-shifted by 8).
    When LMS applies digital volume, the multiplication introduces non-zero noise in
    the LSB byte (byte[0]). The S24 auto-detection in DirettaRingBuffer saw both LSB
    and MSB bytes as non-zero and incorrectly defaulted to LsbAligned, extracting
    bytes [0,1,2] instead of [1,2,3]. This shifted every sample by one byte position,
    producing loud noise.
  - Affected: 24-bit sources (e.g., 96kHz/24-bit FLAC from Qobuz) with digital volume
    control active. 16-bit sources were unaffected (16-bit left-shift by 16 keeps
    byte[0] at zero even with volume applied).
  - Fix: wrapper now hints `MsbAligned` to DirettaSync after each `open()` call.
    `detectS24PackMode()` returns `Deferred` (instead of `LsbAligned`) for the
    ambiguous both-non-zero case, allowing the hint to take precedence.

---

## [1.0.1] - 2026-02-05

### UPGRADE NOTICE
When upgrading from v1.0.0, you must delete the old configuration files
before running install.sh, as the configuration file format has changed:
```bash
sudo systemctl stop squeeze2diretta
sudo rm -f /opt/squeeze2diretta/squeeze2diretta.conf
sudo rm -f /opt/squeeze2diretta/start-squeeze2diretta.sh
```
Then re-run `install.sh`. Your previous settings will need to be re-entered
in the new configuration file.

### Fixed
- **First track crackling/noise** on Audiolinux and RPi systems
  - Root cause: race condition between audio data arriving on stdout and format
    detection via stderr. Diretta was opened at default 44100Hz before the actual
    format was known, causing wrong-format audio to reach the DAC.
  - Fix: wrapper now waits for first track format detection before streaming.
    `g_current_sample_rate` initialized to 0 (was 44100) so the first track
    always triggers the reopen+burst-fill path with the correct format.
- **DSD noise/hiss** caused by byte-order mismatch (PR #3, SwissMountainBear)
  - Squeezelite packs DSD bytes MSB-first into uint32_t but outputs S32_LE,
    so bytes arrive reversed on the pipe. Added byte-swap during de-interleave.
- **PCM buffer margins** increased for better resilience on systems with
  different scheduling characteristics (RT kernels, slower CPUs)
  - `PCM_BUFFER_SECONDS`: 0.3s -> 0.5s
  - `PCM_PREFILL_MS`: 30ms -> 50ms
- **DSD playback broken when `-a 24` was set** (DSD files played as PCM at 352.8kHz)
  - Root cause: wrapper incorrectly converted S32_LE to packed S24_3LE (3 bytes/sample)
    but DirettaSync always expects 4-byte padded input for 24-bit audio.
  - Fix: removed wrapper-side bit depth conversion; DirettaSync handles 32→24/16 conversion
    internally via `configureRingPCM(inputBps=4, direttaBps=3)`.
- **PAUSE_ON_START not working** since start script changes
  - Root cause: `sleep 0.5` was too short; squeezelite hadn't registered with LMS yet.
  - Fix: polls LMS CLI (`players 0 100`) every 2s until player appears (max 30s).
- **Build failure on pre-AVX2 x86 CPUs** (x86-64-v2 variant)
  - Root cause: `DirettaRingBuffer.h` defined `DIRETTA_HAS_AVX2=1` for all x86 platforms,
    causing AVX2 intrinsics to be compiled even without `-mavx2` compiler flag.
  - Fix: guard now requires both x86 platform AND `__AVX2__` compiler define.
  - CPUs without AVX2 (pre-Haswell/Skylake) now use scalar fallbacks correctly.

### Added
- **Burst-fill mechanism** for format transitions (SwissMountainBear)
  - Fills ring buffer at full pipe speed before playback begins
  - Escapes "equilibrium trap" where push rate equals pull rate
- **Clock family detection** for PCM-to-DSD transitions within same 44.1/48kHz family
- **Two-tier close**: lightweight `close()` for format transitions, full `release()` for shutdown
- **`-W` option** (squeezelite WAV/AIFF header parsing)
  - Reads format from WAV/AIFF headers instead of trusting server parameters
  - Configurable via command line (`-W`) or config file (`WAV_HEADER=yes`)
  - Disabled by default (may help with format detection issues on some setups)
  - Suggested by Filippo (GentooPlayer)
- **TARGET_MARCH** cmake option for cross-compilation (for Audiolinux/Piero)
- **ARM64 page size detection** using `getconf PAGESIZE` (Filippo/GentooPlayer)
- **`-a` option** (PCM bit depth) for DACs not supporting 32-bit
  - Sets Diretta output bit depth to 24-bit or 16-bit (DirettaSync handles conversion internally)
  - Configurable via command line (`-a 24`) or config file (`SAMPLE_FORMAT=24`)
  - Default: 32 (no conversion)
- **PAUSE_ON_START** option to pause playback on service start
- **Auto-open config editor** after installation

### Changed
- Reduced buffer sizes for lower latency (SwissMountainBear):
  - DSD: 2.0s -> 0.8s, PCM: 2.0s -> 0.5s
- install.sh refactored to use `cp` from systemd/ directory (SwissMountainBear)
- PCM rate changes now use full SDK close/reopen for clean transitions

---

## [1.0.0] - 2026-02-01

### Added
- Initial public release of squeeze2diretta
- **Native DSD playback** support (DSD64, DSD128, DSD256, DSD512)
  - DSD_U32_BE (Big Endian) format for LMS
  - DSD_U32_LE (Little Endian) format
  - DoP (DSD over PCM) support for Roon compatibility
- **DoP to Native DSD conversion** for Diretta Targets that don't support DoP passthrough
- **High-resolution PCM** support up to 768kHz
- **DirettaSync v2.0** integration from DirettaRendererUPnP
  - Lock-free SPSC ring buffers
  - SIMD optimizations (AVX2/AVX-512)
  - Low-latency architecture
- **Dynamic format detection** via squeezelite stderr monitoring
  - Automatic PCM/DSD mode switching
  - Sample rate change detection
- **Smooth format transitions** with silence padding
  - PCM to DSD: 5 silence buffers + 50ms delay
  - DSD to PCM: 8 silence buffers + 80ms delay
  - Non-blocking pipe drain to avoid audio truncation
- **Interactive installer** (install.sh)
  - Full installation with systemd service
  - Squeezelite setup with native DSD patch
  - Network optimization (MTU, buffers)
  - Configuration file generation
- **Multi-architecture support**
  - x86_64: v2 (baseline), v3 (AVX2), v4 (AVX-512), zen4 (AMD Ryzen 7000+)
  - ARM64: standard and k16 (16KB pages) variants
  - RISC-V: experimental support
- **Systemd service** with EnvironmentFile configuration
- **DSD format configuration** via config file (u32be, u32le, dop)

### Architecture
- Wrapper launches squeezelite with stdout output
- Reads PCM/DSD audio from pipe
- Converts interleaved to planar format for DSD
- Streams to Diretta Target using DIRETTA::Sync API

### Dependencies
- Diretta Host SDK 147 or 148 (must be downloaded separately)
- Squeezelite with native DSD support (setup script included)
- CMake 3.10+, C++17 compiler

### Credits
- **Dominique COMET** - squeeze2diretta development
- **SwissMountainBear** - DIRETTA::Sync architecture (from MPD plugin)
- **leeeanh** - Performance optimizations (lock-free buffers, SIMD)
- **Yu Harada** - Diretta Protocol & SDK
- **Ralph Irving** - Squeezelite

### License
- MIT License for squeeze2diretta code
- Diretta SDK: Personal use only (proprietary)
- Squeezelite: GPL v3 (launched as separate process)

---

[2.0.2]: https://github.com/cometdom/squeeze2diretta/releases/tag/v2.0.2
[2.0.1]: https://github.com/cometdom/squeeze2diretta/releases/tag/v2.0.1
[2.0.0]: https://github.com/cometdom/squeeze2diretta/releases/tag/v2.0.0
[1.0.2]: https://github.com/cometdom/squeeze2diretta/releases/tag/v1.0.2
[1.0.1]: https://github.com/cometdom/squeeze2diretta/releases/tag/v1.0.1
[1.0.0]: https://github.com/cometdom/squeeze2diretta/releases/tag/v1.0.0
