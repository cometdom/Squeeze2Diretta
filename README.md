# squeeze2diretta

A lightweight Squeezebox client that streams directly to Diretta protocol DACs, bypassing traditional audio outputs.

## Overview

**squeeze2diretta** is a fork of [squeezelite](https://github.com/ralph-irving/squeezelite) with a native Diretta output backend. It connects to Lyrion Music Server (formerly Logitech Media Server) and streams audio directly to Diretta-compatible DACs over the network.

### Why squeeze2diretta?

- **Direct streaming**: LMS → squeeze2diretta → Diretta DAC (no UPnP layer)
- **Bit-perfect playback**: Raw PCM/DSD data sent directly to DAC
- **Native gapless**: Full support for gapless playback
- **All formats supported**: FLAC, MP3, OGG, AAC, ALAC, WMA, DSD
- **Low latency**: Minimal processing overhead
- **Better than squeeze2upnp**: No double conversion (Squeezebox→UPnP→Diretta)

## Features

- ✅ Native Diretta protocol output
- ✅ Support for PCM up to 768kHz/32-bit
- ✅ Native DSD support (DSD64/128/256/512/1024)
- ✅ Gapless playback
- ✅ Buffer size configuration
- ✅ Jumbo frames support (MTU up to 16128)
- ✅ Advanced Diretta SDK tuning (thread mode, cycle time, etc.)
- ✅ Automatic Diretta target discovery
- ✅ Volume control (software)
- ✅ Resampling support (via SoX)

## Requirements

### System Requirements

- **Linux** (tested on Ubuntu 22.04/24.04, Fedora 40, AudioLinux)
- **Lyrion Music Server** (LMS) running on your network
- **Diretta-compatible DAC** (e.g., Holo Audio Spring, May, etc.)

### Diretta Host SDK

⚠️ **IMPORTANT**: The Diretta Host SDK is **proprietary software** by Yu Harada and is **NOT included** in this repository.

**To obtain the SDK:**

1. Visit the official Diretta SDK page: **https://www.diretta.link/hostsdk.html**
2. Download the appropriate version for your platform (Linux x64/ARM)
3. Extract the SDK to `squeeze2diretta/diretta-sdk/`

**Expected directory structure after SDK installation:**

```
squeeze2diretta/
├── diretta-sdk/
│   ├── include/
│   │   ├── DIRETTA/
│   │   │   ├── Sync.h
│   │   │   └── ...
│   │   └── ACQUA/
│   │       ├── Clock.h
│   │       └── ...
│   └── lib/
│       ├── libDIRETTA.so
│       └── libACQUA.so
├── diretta/
│   ├── DirettaOutputSimple.h
│   └── DirettaOutputSimple.cpp
└── ...
```

**Without the SDK, compilation will fail.** Please ensure you have obtained and installed the SDK before attempting to build.

### Build Dependencies

```bash
# Ubuntu/Debian
sudo apt-get install build-essential cmake git \
                     libflac-dev libmad0-dev libvorbis-dev \
                     libfaad-dev libmpg123-dev libasound2-dev

# Fedora
sudo dnf install gcc-c++ cmake git \
                 flac-devel libmad-devel libvorbis-devel \
                 faad2-devel mpg123-devel alsa-lib-devel

# Optional: For resampling support
sudo apt-get install libsoxr-dev  # Ubuntu/Debian
sudo dnf install soxr-devel       # Fedora

# Optional: For ALAC/WMA support
sudo apt-get install libavformat-dev libavcodec-dev  # Ubuntu/Debian
sudo dnf install ffmpeg-devel                        # Fedora
```

## Installation

### 1. Clone Repository

```bash
git clone --recursive https://github.com/cometdom/squeeze2diretta.git
cd squeeze2diretta
```

### 2. Install Diretta SDK

Download from https://www.diretta.link/hostsdk.html and extract:

```bash
# Example for Linux x64
tar -xzf DirettaHostSDK_vX.X.X_Linux_x64.tar.gz
mv DirettaHostSDK diretta-sdk
```

### 3. Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 4. Install (optional)

```bash
sudo make install
```

Or run directly from build directory:
```bash
./squeeze2diretta -l  # List Diretta targets
```

## Quick Start

```bash
# List available Diretta targets
squeeze2diretta -l

# Connect to LMS with first Diretta target
squeeze2diretta -s <lms-ip> -n "Living Room" -t 1

# With custom buffer (recommended for hi-res)
squeeze2diretta -s <lms-ip> -n "Living Room" -t 1 -b 3.0
```

## Usage

```
squeeze2diretta [options]

Diretta Options:
  -t <number>           Diretta target number (use -l to list)
  -l                    List available Diretta targets and exit
  -b <seconds>          Buffer size in seconds (default: 2.0)
  --thread-mode <n>     THRED_MODE bitmask (default: 1)
  --cycle-time <µs>     Transfer cycle max time (default: 10000)
  --cycle-min-time <µs> Transfer cycle min time (default: 333)
  --info-cycle <µs>     Info packet cycle time (default: 5000)
  --mtu <bytes>         Override MTU (default: auto-detect)

Squeezebox Options:
  -s <server>[:<port>]  Connect to LMS server (default: autodiscovery)
  -n <name>             Set player name
  -m <mac>              Set MAC address (format: ab:cd:ef:12:34:56)
  -M <modelname>        Set model name (default: SqueezeLite)

Audio Options:
  -c <codec1>,<codec2>  Restrict codecs (flac,pcm,mp3,ogg,aac,dsd...)
  -e <codec1>,<codec2>  Exclude codecs
  -r <rates>            Supported sample rates
  -u [params]           Enable upsampling (SoX resampler)
  -D [delay][:format]   DSD output mode

Logging:
  -d <log>=<level>      Set logging level
                        logs: all|slimproto|stream|decode|output
                        level: info|debug|sdebug
  -f <logfile>          Write logs to file

Other:
  -z                    Run as daemon
  -P <pidfile>          Write PID to file
  -?                    Show help
```

## Configuration Examples

### Basic Setup (Auto-discovery)
```bash
squeeze2diretta -n "Diretta Player" -t 1
```

### Connect to Specific LMS Server
```bash
squeeze2diretta -s 192.168.1.100 -n "Living Room" -t 1
```

### High-Resolution Audio (DSD512, PCM768)
```bash
squeeze2diretta -s 192.168.1.100 -n "HiFi Room" -t 1 \
                -b 3.0 --thread-mode 17 --cycle-time 8000
```

### With Upsampling to 192kHz
```bash
squeeze2diretta -s 192.168.1.100 -n "Living Room" -t 1 \
                -u vLIX::28:95:100:50 -Z 192000
```

### DSD Native Output
```bash
squeeze2diretta -s 192.168.1.100 -n "DSD Player" -t 1 \
                -D 0:dsd -b 3.0
```

### Run as Daemon with Logging
```bash
squeeze2diretta -s 192.168.1.100 -n "Diretta Player" -t 1 \
                -z -f /var/log/squeeze2diretta.log
```

## Systemd Service

Create `/etc/systemd/system/squeeze2diretta.service`:

```ini
[Unit]
Description=squeeze2diretta Squeezebox Player
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
ExecStart=/usr/local/bin/squeeze2diretta -s 192.168.1.100 -n "Diretta Player" -t 1 -z
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

Enable and start:
```bash
sudo systemctl enable squeeze2diretta
sudo systemctl start squeeze2diretta
sudo systemctl status squeeze2diretta
```

View logs:
```bash
sudo journalctl -u squeeze2diretta -f
```

## Advanced Diretta Configuration

### Thread Mode (--thread-mode)

Bitmask for real-time thread behavior:

| Value | Flag | Description |
|-------|------|-------------|
| 1 | Critical | REALTIME priority (default) |
| 2 | NoShortSleep | Disable short sleep intervals |
| 4 | NoSleep4Core | Disable sleep for 4-core systems |
| 8 | SocketNoBlock | Non-blocking socket operations |
| 16 | OccupiedCPU | Maximize CPU utilization |
| 32/64/128 | FEEDBACK | Moving average feedback control |

**Examples:**
- `--thread-mode 1` - Default (Critical only)
- `--thread-mode 17` - Critical + OccupiedCPU (high performance)
- `--thread-mode 33` - Critical + FEEDBACK32

## Comparison with Alternatives

| Feature | squeeze2diretta | squeeze2upnp | DirettaRendererUPnP |
|---------|----------------|--------------|---------------------|
| Protocol | Squeezebox → Diretta | Squeezebox → UPnP → Diretta | UPnP → Diretta |
| Layers | 2 | 3 | 2 |
| Gapless | Native | Depends | Yes |
| Latency | Lowest | Medium | Low |
| LMS Integration | Perfect | Perfect | Via UPnP |
| Setup | Simple | Medium | Medium |
| DSD Support | Native | Native | Native |

## Troubleshooting

### Player not appearing in LMS

1. Check squeeze2diretta is running:
```bash
ps aux | grep squeeze2diretta
```

2. Check network connectivity:
```bash
ping <lms-ip>
```

3. Try specifying LMS server explicitly:
```bash
squeeze2diretta -s <lms-ip> -n "Test Player" -t 1 -d all=info
```

### No Diretta targets found

1. Ensure DAC is powered on and connected to network
2. Check firewall settings (UDP broadcast must be allowed)
3. Verify network connectivity:
```bash
squeeze2diretta -l
```

### Audio dropouts or stuttering

1. Increase buffer size:
```bash
squeeze2diretta -b 3.0
```

2. Adjust thread mode:
```bash
squeeze2diretta --thread-mode 17
```

3. Enable jumbo frames if supported:
```bash
squeeze2diretta --mtu 9000
```

### Build fails with "DIRETTA not found"

Ensure Diretta SDK is properly installed:
```bash
ls -la diretta-sdk/lib/
# Should show: libDIRETTA.so, libACQUA.so
```

## Known Limitations

- **Audirvana compatibility**: Not applicable (LMS only)
- **Multi-room sync**: Not yet implemented
- **Hardware volume control**: Not supported (use LMS volume)

## Development

### Project Structure

```
squeeze2diretta/
├── diretta/
│   ├── DirettaOutputSimple.h    # Simplified Diretta output
│   └── DirettaOutputSimple.cpp
├── output_diretta.c              # Squeezelite output backend
├── squeezelite/                  # Submodule
└── diretta-sdk/                  # Downloaded separately
```

### Contributing

Contributions welcome! Please:
1. Fork the repository
2. Create a feature branch
3. Submit a pull request

## Credits

- **Squeezelite**: Adrian Smith (original), Ralph Irving (maintainer)
- **Diretta Protocol & SDK**: Yu Harada - https://www.diretta.link/
- **squeeze2diretta**: Dominique COMET

## License

GPLv3 - See LICENSE file for details

**Note:** This project combines:
- Squeezelite code (GPLv3)
- Diretta SDK (proprietary - separate download required)

The Diretta SDK is proprietary software and must be obtained separately from https://www.diretta.link/hostsdk.html

## Related Projects

- [DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP) - UPnP renderer for Diretta
- [Lyrion Music Server](https://lyrion.org/) - Music server (formerly LMS)
- [Squeezelite](https://github.com/ralph-irving/squeezelite) - Original player

## Support

- Issues: https://github.com/cometdom/squeeze2diretta/issues
- Diretta SDK: https://www.diretta.link/hostsdk.html
- LMS Forums: https://forums.slimdevices.com/
