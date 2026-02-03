# squeeze2diretta v1.0.0

**Squeezelite to Diretta Bridge - Native DSD & Hi-Res PCM Streaming**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-Linux-blue.svg)](https://www.linux.org/)
[![C++17](https://img.shields.io/badge/C++-17-00599C.svg)](https://isocpp.org/)

---

![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)
![DSD](https://img.shields.io/badge/DSD-Native-green.svg)
![SDK](https://img.shields.io/badge/SDK-DIRETTA::Sync-orange.svg)

---

## Support This Project

If you find this tool valuable, you can support development:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/cometdom)

**Important notes:**
- Donations are **optional** and appreciated
- Help cover test equipment and coffee
- **No guarantees** for features, support, or timelines
- The project remains free and open source for everyone

---

## IMPORTANT - PERSONAL USE ONLY

This tool uses the **Diretta Host SDK**, which is proprietary software by Yu Harada available for **personal use only**. Commercial use is strictly prohibited. See [LICENSE](LICENSE) for details.

---

## Overview

**squeeze2diretta** bridges Lyrion Music Server (LMS) and Squeezelite to Diretta protocol endpoints, enabling bit-perfect playback of high-resolution PCM and native DSD through Diretta Targets. It uses the advanced DirettaSync implementation from DirettaRendererUPnP v2.0 for low-latency, high-quality audio streaming.

### What is This?

This tool acts as a **wrapper** that:
1. Launches **Squeezelite** with stdout output
2. Reads PCM/DSD audio data from Squeezelite
3. Streams it to a **Diretta Target** using the Diretta protocol
4. Handles format changes dynamically (PCM ↔ DSD, different sample rates)

### Why Use This?

- **Native DSD playback** from your LMS library (DSF/DFF files)
- **Bit-perfect streaming** - bypasses OS audio stack
- **High-resolution PCM** up to 768kHz (limited by Squeezelite)
- **Seamless integration** with existing LMS/Squeezelite setup
- **Low latency** using DirettaSync v2.0 architecture

---

## Architecture

```
+---------------------------+
|  Lyrion Music Server    |  (LMS on any network device)
+-------------+-------------+
              |
              | HTTP Streaming
              v
+-------------------------------------------------------------+
|  squeeze2diretta                                            |
|                                                             |
|  +------------------+        +---------------------------+  |
|  |   Squeezelite    | -----> |      DirettaSync          |  |
|  |   (decoder)      | stdout |  (from DirettaRendererUPnP)|  |
|  |                  |  pipe  |                           |  |
|  |  - DSF/DFF->DSD  |        |  - Format conversion      |  |
|  |  - FLAC->PCM     |        |  - SIMD optimizations     |  |
|  |  - Rate change   |        |  - Low-latency buffers    |  |
|  +------------------+        +-------------+-------------+  |
|                                            |                |
+--------------------------------------------+----------------+
                                             |
                        Diretta Protocol (UDP/Ethernet)
                                             |
                                             v
                         +-------------------+-------------------+
                         |           Diretta TARGET             |
                         |  (Memory Play, GentooPlayer, DDC-0)  |
                         +-------------------+-------------------+
                                             |
                                             v
                                    +--------+--------+
                                    |       DAC       |
                                    +-----------------+
```

---

## Features

### Audio Quality
- **Native DSD support**: DSD64, DSD128, DSD256 (DSF and DFF files)
- **High-resolution PCM**: Up to 768kHz (Squeezelite limitation)
- **Bit-perfect streaming**: No resampling when formats match
- **Format support**: All formats supported by Squeezelite (FLAC, ALAC, WAV, AIFF, DSF, DFF, MP3, AAC, OGG)
- **Gapless playback**: Seamless album listening

### DSD Handling
- **Native DSD streaming**: Direct DSD bitstream (not DoP)
- **Automatic format detection**: DSD_U32_BE, DSD_U32_LE from Squeezelite
- **Dynamic conversion**:
  - Interleaved → Planar conversion
  - Big Endian → Little Endian byte swap
  - LSB/MSB bit order handling (automatic via DirettaSync)
- **Optimized for DSF files**: Common audiophile format

### Low-Latency Architecture
- **DirettaSync v2.0**: Lock-free ring buffers, SIMD optimizations
- **Direct pipe**: Squeezelite stdout → squeeze2diretta (minimal overhead)
- **Dynamic rate limiting**: Precise timing for smooth playback

### Network Optimization
- **Adaptive packet sizing**: Synchronized with Diretta SDK
- **Jumbo frame support**: Up to 16KB MTU
- **Automatic MTU detection**: Optimal performance configuration

---

## Requirements

### Supported Architectures

Same as DirettaRendererUPnP - the build system automatically detects your CPU:

| Architecture | Variants | Notes |
|--------------|----------|-------|
| **x64 (Intel/AMD)** | v2 (baseline), v3 (AVX2), v4 (AVX-512), zen4 | AVX2 recommended |
| **ARM64** | Standard (4KB pages), k16 (16KB pages) | Raspberry Pi 4/5 supported |
| **RISC-V** | Experimental | riscv64 |

### Platform Support

| Platform | Status |
|----------|--------|
| **Linux x64** | Fully supported (Fedora, Ubuntu, Arch, AudioLinux) |
| **Linux ARM64** | Fully supported (Raspberry Pi 4/5) |
| **Windows** | Not supported |
| **macOS** | Not supported |

### Hardware
- **Minimum**: Dual-core CPU, 1GB RAM, Gigabit Ethernet
- **Recommended**: Quad-core CPU, 2GB RAM, 2.5/10G Ethernet with jumbo frames
- **Network**: Gigabit Ethernet minimum (10G recommended for DSD256+)
- **MTU**: 1500 minimum, jumbo frames recommended (9014 or 16128, must match Diretta Target)

### Software Requirements
- **OS**: Linux with kernel 4.x+ (RT kernel recommended)
- **Diretta Host SDK**: Version 148 or 147 ([download here](https://www.diretta.link/hostsdk.html))
- **Squeezelite**: Compiled with native DSD support (included setup script)
- **LMS**: Lyrion Music Server running on your network
- **Build tools**: gcc/g++ 7.0+, make, CMake 3.10+

---

## Quick Start

### Option A: Interactive Installer (Recommended)

The easiest way to install squeeze2diretta is using the interactive installer:

```bash
# 1. Download Diretta Host SDK first
#    Visit: https://www.diretta.link/hostsdk.html
#    Extract to: ~/DirettaHostSDK_148

# 2. Clone repository
git clone https://github.com/cometdom/squeeze2diretta.git
cd squeeze2diretta

# 3. Run interactive installer
chmod +x install.sh
./install.sh
```

> **Tip: Transferring files from Windows to Linux**
>
> If you downloaded the SDK on Windows and need to transfer it to your Linux machine:
>
> **Using PowerShell or CMD** (OpenSSH is built into Windows 10/11):
> ```powershell
> # Transfer the SDK archive to your Linux machine
> # Replace with actual filename (e.g., DirettaHostSDK_148_5.tar.zst)
> scp C:\Users\YourName\Downloads\DirettaHostSDK_XXX_Y.tar.zst user@linux-ip:~/
> ```
>
> **Using WSL** (Windows Subsystem for Linux):
> ```bash
> # Windows files are accessible under /mnt/c/
> cp /mnt/c/Users/YourName/Downloads/DirettaHostSDK_*.tar.zst ~/
> ```
>
> Then extract on Linux:
> ```bash
> cd ~
> tar --zstd -xf DirettaHostSDK_*.tar.zst
> ```

The installer provides an interactive menu with options for:
- **Full installation** (recommended) - Everything in one go
- **Setup Squeezelite only** - Download, patch, and compile Squeezelite
- **Build only** - Compile squeeze2diretta (if dependencies are already installed)
- **Install systemd service** - Set up automatic startup
- **Configure network** - MTU, buffers, and firewall
- **Aggressive optimization** (Fedora only) - For dedicated audio servers

**Command-line options:**
```bash
./install.sh --full       # Full installation (non-interactive)
./install.sh --build      # Build only
./install.sh --service    # Install systemd service only
./install.sh --help       # Show all options
```

---

### Option B: Manual Installation

If you prefer manual control, follow these steps:

#### 1. Install System Dependencies

**Fedora:**
```bash
sudo dnf install -y gcc-c++ make cmake git patch \\
    alsa-lib-devel flac-devel libvorbis-devel \\
    libmad-devel mpg123-devel opus-devel soxr-devel openssl-devel
```

**Ubuntu/Debian:**
```bash
sudo apt install -y build-essential cmake git patch \\
    libasound2-dev libflac-dev libvorbis-dev \\
    libmad0-dev libmpg123-dev libopus-dev libsoxr-dev libssl-dev
```

**Arch/AudioLinux:**
```bash
sudo pacman -S base-devel cmake git patch \\
    alsa-lib flac libvorbis libmad mpg123 opus soxr openssl
```

#### 2. Download Diretta Host SDK

1. Visit [diretta.link](https://www.diretta.link/hostsdk.html)
2. Download **DirettaHostSDK_148** (or latest version)
3. Extract to one of these locations:
   - \`~/DirettaHostSDK_148\`
   - \`/opt/DirettaHostSDK_148\`
   - Or set \`DIRETTA_SDK_PATH\` environment variable

#### 3. Clone Repository

```bash
git clone https://github.com/cometdom/squeeze2diretta.git
cd squeeze2diretta
```

#### 4. Setup Squeezelite

```bash
# Run automated setup (downloads, patches, compiles)
chmod +x setup-squeezelite.sh
./setup-squeezelite.sh
```

**What the script does:**
1. Clones Squeezelite from official repository
2. Applies stdout flush patch (required for pipe mode)
3. Compiles with native DSD support enabled
4. Creates \`squeezelite/squeezelite\` binary ready to use

#### 5. Build squeeze2diretta

```bash
# Create build directory
mkdir build && cd build

# Configure and build (auto-detects SDK and architecture)
cmake ..
make

# Binary created at: build/squeeze2diretta
```

**Architecture override** (if auto-detection fails):
```bash
cmake -DARCH_NAME=x64-linux-15v3 ..      # For x64 with AVX2
cmake -DARCH_NAME=aarch64-linux-15k16 .. # For Raspberry Pi
```

### 6. Find Your Diretta Target

```bash
# List available Diretta targets on network
./build/squeeze2diretta --list-targets
```

Output example:
```
Found 2 Diretta target(s):
  [1] DDC-0_8A60 (192.168.1.50)
  [2] GentooPlayer_AB12 (192.168.1.51)
```

### 7. Run squeeze2diretta

```bash
# Basic usage (replace with your LMS server IP and target number)
./build/squeeze2diretta \\
    --squeezelite ./squeezelite/squeezelite \\
    -s 192.168.1.100 \\
    --target 1

# With verbose output (for troubleshooting)
./build/squeeze2diretta \\
    --squeezelite ./squeezelite/squeezelite \\
    -s 192.168.1.100 \\
    --target 1 \\
    -v

# Specify sample rate restrictions (e.g., 44.1kHz family only)
./build/squeeze2diretta \\
    --squeezelite ./squeezelite/squeezelite \\
    -r 768000 \\
    -s 192.168.1.100 \\
    --target 1
```

### 8. Connect from LMS

1. Open LMS web interface (usually http://lms-server:9000)
2. Go to Settings → Player → Audio
3. You should see "squeeze2diretta" as a player
4. Select it and start playing music!

---

## Configuration Options

### squeeze2diretta Options

```bash
--squeezelite <path>    Path to squeezelite binary (required)
--target, -t <index>    Select Diretta target by index (required)
--list-targets          List available Diretta targets and exit
--verbose, -v           Enable verbose debug output
```

### Squeezelite Options (passed through)

Common options that squeeze2diretta passes to Squeezelite:

```bash
-s <server>            LMS server IP address
-n <name>              Player name (default: squeeze2diretta)
-M <model>             Model name (default: SqueezeLite)
-r <rates>             Supported sample rates (e.g., 768000)
-D :u32be              DSD output format (u32be = Big Endian U32)
-d <categories>        Debug output (e.g., all=info)
```

### Configuration File (squeeze2diretta.conf)

When using the systemd service, all settings are stored in `/etc/squeeze2diretta.conf`. Edit this file to customize your installation:

```bash
sudo nano /etc/squeeze2diretta.conf
```

**Key settings:**

| Setting | Description | Default |
|---------|-------------|---------|
| `LMS_SERVER` | IP address of your LMS server | `192.168.1.100` |
| `TARGET` | Diretta target number (use `--list-targets` to find) | `1` |
| `PLAYER_NAME` | Name shown in LMS web interface | `squeeze2diretta` |
| `MAX_SAMPLE_RATE` | Maximum sample rate in Hz | `768000` |
| `DSD_FORMAT` | DSD output format (see below) | `u32be` |
| `PAUSE_ON_START` | Pause playback when service starts (prevents auto-resume) | `no` |
| `VERBOSE` | Set to `-v` for debug output | (empty) |

### DSD Format: LMS vs Roon

The `DSD_FORMAT` setting is critical for proper DSD playback:

| Source | DSD_FORMAT | Description |
|--------|------------|-------------|
| **LMS (Lyrion Music Server)** | `u32be` | Native DSD Big Endian. LMS sends true DSD data. |
| **Roon** | `dop` | DoP (DSD over PCM). Roon's Squeezebox emulation only supports DoP. |

**For LMS users:**
```bash
# In /etc/squeeze2diretta.conf
DSD_FORMAT=u32be
```
LMS can send native DSD directly to Squeezelite, providing the best quality path.

**For Roon users:**
```bash
# In /etc/squeeze2diretta.conf
DSD_FORMAT=dop
```
Roon's Squeezebox protocol emulation has limitations and sends DSD as DoP (DSD over PCM). squeeze2diretta automatically converts DoP back to native DSD for the Diretta Target.

**After editing, restart the service:**
```bash
sudo systemctl restart squeeze2diretta
```

---

## Systemd Service (Auto-Start)

See [systemd/README.md](systemd/README.md) for detailed instructions on setting up auto-start with systemd.

**Quick example:**

```bash
# Install service file
sudo cp systemd/squeeze2diretta.service /etc/systemd/system/

# Edit to match your setup
sudo nano /etc/systemd/system/squeeze2diretta.service

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable --now squeeze2diretta

# Check status
sudo systemctl status squeeze2diretta
```

---

## Credits

### Author
**Dominique COMET** ([@cometdom](https://github.com/cometdom)) - squeeze2diretta development

### Core Technologies

- **DirettaSync v2.0** - From [DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP)
  - Low-latency architecture by Dominique COMET
  - Core Diretta integration by **SwissMountainsBear** (ported from [MPD Diretta Output Plugin](https://github.com/swissmountainsbear/mpd-diretta-output-plugin))
  - Performance optimizations by **leeeanh** (lock-free ring buffers, SIMD, cache-line separation)

- **Diretta Protocol & SDK** - **Yu Harada** ([diretta.link](https://www.diretta.link))

- **Squeezelite** - **Ralph Irving** and contributors ([GitHub](https://github.com/ralph-irving/squeezelite))

- **Lyrion Music Server** - Open source audio streaming server

### Special Thanks

- **SwissMountainsBear** - For the \`DIRETTA::Sync\` architecture, \`getNewStream()\` callback implementation, and buffer management patterns from his MPD plugin that made DirettaSync possible

- **leeeanh** - For brilliant optimization strategies including lock-free SPSC ring buffers, power-of-2 sizing with bitmask modulo, cache-line separation, and AVX2 SIMD batch conversions

- **Yu Harada** - Creator of Diretta protocol and SDK, guidance on low-level API usage

- **Audiophile community** - Testing and feedback on DSD playback

---

## License

This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for details.

**IMPORTANT**: The Diretta Host SDK is proprietary software by Yu Harada and is licensed for **personal use only**. Commercial use is prohibited.

---

**Enjoy native DSD and hi-res PCM streaming from your LMS library!**

*Last updated: 2026-02-01*
