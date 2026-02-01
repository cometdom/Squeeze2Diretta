# Changelog

All notable changes to squeeze2diretta will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

[1.0.0]: https://github.com/cometdom/squeeze2diretta/releases/tag/v1.0.0
