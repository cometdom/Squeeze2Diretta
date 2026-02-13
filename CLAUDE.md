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
Data Flow (v2.0):
LMS (network)
  → Squeezelite (patched, decodes to stdout with SQFH headers)
    → stdout pipe: [SQFH header][audio data][SQFH header][audio data]...
      → squeeze2diretta-wrapper (main process)
        → Reads headers synchronously (no stderr parsing)
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

## Format Change Handling (v2.0)

### In-Band Format Signaling

v2.0 uses a patched squeezelite that writes a **16-byte binary header** ("SQFH") to stdout
at each track boundary. The wrapper reads this header **synchronously** from the pipe,
eliminating the race condition of v1.x's async stderr log parsing.

### Header Format (16 bytes)

```c
struct sq_format_header {
    uint8_t  magic[4];       // "SQFH"
    uint8_t  version;        // 1
    uint8_t  channels;       // 2
    uint8_t  bit_depth;      // PCM: 16/24/32, DSD: 1, DoP: 24
    uint8_t  dsd_format;     // 0=PCM, 1=DOP, 2=DSD_U32_LE, 3=DSD_U32_BE
    uint32_t sample_rate;    // LE, Hz
    uint8_t  reserved[4];
};
```

### Flow

1. Wrapper blocks on `readExact(16)` — waits for header
2. Validates "SQFH" magic
3. Compares with current format — if changed, closes and reopens Diretta
4. If same format (gapless), continues streaming without reopen
5. Burst-fills ring buffer, then streams with rate limiting
6. Peeks for next header before each audio read

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
