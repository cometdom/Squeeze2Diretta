# Changelog

All notable changes to squeeze2diretta will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
  - Truncates squeezelite 32-bit output to 24-bit or 16-bit before streaming
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

[1.0.1]: https://github.com/cometdom/squeeze2diretta/releases/tag/v1.0.1
[1.0.0]: https://github.com/cometdom/squeeze2diretta/releases/tag/v1.0.0
