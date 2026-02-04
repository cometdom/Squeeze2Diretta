# CLAUDE.md - squeeze2diretta

This file provides guidance to Claude Code when working with this repository.

## Project Overview

**squeeze2diretta** is a bridge application connecting Lyrion Music Server (LMS) / Squeezelite to Diretta protocol endpoints. It enables bit-perfect playback of native DSD (up to DSD512) and high-resolution PCM (up to 768kHz) via the Diretta protocol.

**Architecture**: Wrapper approach (not UPnP) - launches Squeezelite with stdout output and pipes audio to DirettaSync.

## Build Commands

```bash
# Build (auto-detects architecture and SDK)
mkdir build && cd build
cmake ..
make

# Specific architecture variants
cmake -DARCH_NAME=x64-linux-15v3 ..       # x64 AVX2 (most common)
cmake -DARCH_NAME=aarch64-linux-15 ..     # Raspberry Pi 4
cmake -DARCH_NAME=aarch64-linux-15k16 ..  # Raspberry Pi 5 (16KB pages)

# Custom SDK path
export DIRETTA_SDK_PATH=/path/to/DirettaHostSDK_148
cmake ..
```

## Running

```bash
# List available Diretta targets
sudo ./squeeze2diretta --list-targets

# Basic usage
sudo ./squeeze2diretta -s <lms-ip> --target 1

# Verbose mode for debugging
sudo ./squeeze2diretta -s <lms-ip> --target 1 -v

# With specific squeezelite path
sudo ./squeeze2diretta --squeezelite /path/to/squeezelite -s <lms-ip> --target 1
```

## Architecture

```
Data Flow:
LMS (network)
  → Squeezelite (child process, decodes to stdout)
    → squeeze2diretta-wrapper (main process)
      → Detects format changes via stderr monitoring
      → Converts DSD interleaved→planar, DoP→native
      → DirettaSync (ring buffer + SDK)
        → Diretta Target (UDP/Ethernet)
          → DAC
```

**Key Components**:

| File | Purpose |
|------|---------|
| `squeeze2diretta-wrapper.cpp` | Main orchestrator, format detection, DSD conversion |
| `diretta/DirettaSync.cpp/h` | Diretta SDK wrapper (from DirettaRendererUPnP v2.0) |
| `diretta/DirettaRingBuffer.h` | Lock-free SPSC ring buffer |
| `diretta/globals.cpp/h` | Logging configuration |
| `diretta/FastMemcpy*.h` | SIMD memory operations |

## Critical: Format Change Handling

### The Challenge

Format changes are detected **asynchronously** via Squeezelite's stderr logs:
```
[timestamp] track start sample rate: 352800
[timestamp] format: DSD_U32_BE
```

This creates a **race condition**: by the time the log is detected, the pipe may already contain mixed old/new format data.

### Current Flow (wrapper.cpp:571-738)

1. Stderr monitor thread detects format change log
2. Sets `g_need_reopen = true`
3. Main loop drains pipe (up to 64KB)
4. Calls `g_diretta->open(newFormat)`
5. Continues reading expecting new format

### Known Issue: DSD512 ↔ High-rate PCM

Direct transitions between DSD512 (22.5MHz) and high-rate PCM (352.8kHz) fail silently. Going through 44.1kHz first works. This is due to:
- Race condition between log detection and actual data transition
- Target may need specific reset sequence for extreme rate jumps

### Comparison with DirettaRendererUPnP-X

| Aspect | squeeze2diretta | DirettaRendererUPnP-X |
|--------|-----------------|----------------------|
| Detection | Async (stderr logs) | Sync (audio callback) |
| Timing | Log arrives AFTER data change | Callback has correct frame |
| Data integrity | Race condition possible | Always synchronized |

## Code Style

- **C++17** standard
- **Classes**: `PascalCase`
- **Functions**: `camelCase`
- **Members**: `m_camelCase`
- **Constants**: `UPPER_SNAKE_CASE`
- **Globals**: `g_camelCase`
- **Indentation**: 4 spaces

## DSD Format Handling

**Native DSD (U32_BE from LMS)**:
- Squeezelite outputs interleaved: `[L0][R0][L1][R1]...`
- DirettaSync expects planar: `[L0 L1...][R0 R1...]`
- Wrapper de-interleaves (wrapper.cpp:871-925)

**DoP (from Roon)**:
- Squeezelite outputs S32_LE with DSD bits embedded
- Wrapper extracts DSD bits and converts to planar (wrapper.cpp:822-869)

## Dependencies

- **Diretta Host SDK v147 or v148** (proprietary, not committed)
- **Squeezelite** binary with stdout support
- **POSIX threads** (pthreads)
- **C++17 runtime**

SDK locations searched (in order):
1. `$DIRETTA_SDK_PATH`
2. `~/DirettaHostSDK_148` or `~/DirettaHostSDK_147`
3. `./DirettaHostSDK_148` or `./DirettaHostSDK_147`
4. `/opt/DirettaHostSDK_148` or `/opt/DirettaHostSDK_147`

## Testing

No automated tests. Manual testing with:
- LMS server with various format files
- DSD64 through DSD512
- PCM 44.1kHz through 768kHz
- Format transitions (especially DSD↔PCM)

## Important Notes

- Requires root/sudo for real-time thread priority
- Linux only
- Diretta SDK is personal-use only - never commit SDK files
- Format transition issues are known - workaround is to transition through 44.1kHz
