# Squeezelite Setup for squeeze2diretta

## Overview

squeeze2diretta v2.0 requires a **patched squeezelite** that emits 16-byte binary format headers ("SQFH") to stdout at track boundaries. This enables synchronous format detection, eliminating the race conditions of v1.x's stderr log parsing.

The easiest way to set up squeezelite is using the automated script:

```bash
./setup-squeezelite.sh
```

## Manual Setup

### 1. Clone squeezelite

```bash
git clone https://github.com/ralph-irving/squeezelite.git
cd squeezelite
```

### 2. Apply the v2.0 format header patch

```bash
git apply ../squeezelite-format-header.patch
```

Or if `git apply` fails:
```bash
patch -p1 < ../squeezelite-format-header.patch
```

### 3. Compile with DSD support

On Debian/Ubuntu:
```bash
sudo apt install build-essential libasound2-dev libflac-dev libvorbis-dev \
                 libmad0-dev libmpg123-dev libopus-dev libsoxr-dev libssl-dev
make OPTS="-DDSD -DRESAMPLE -DNO_FAAD"
```

On Fedora/RHEL:
```bash
sudo dnf install gcc make alsa-lib-devel flac-devel libvorbis-devel \
                 libmad-devel mpg123-devel opus-devel soxr-devel openssl-devel
make OPTS="-DDSD -DRESAMPLE -DNO_FAAD"
```

### 4. Install

```bash
sudo cp squeezelite /usr/local/bin/
```

Or copy to your preferred location specified in squeeze2diretta configuration.

## Verification

Test that the patched squeezelite emits format headers:

```bash
./squeezelite -o - -s <LMS_IP> 2>/dev/null | head -c 16 | od -A x -t x1
```

You should see `53 51 46 48` (ASCII "SQFH") as the first 4 bytes.
