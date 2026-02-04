# Systemd Service Configuration

This directory contains systemd service files for running squeeze2diretta as a system service with automatic startup.

## Files

| File | Description |
|------|-------------|
| `squeeze2diretta.service` | Systemd unit file |
| `squeeze2diretta.conf` | Configuration file template |
| `start-squeeze2diretta.sh` | Wrapper script that reads config |

## Quick Setup (Recommended)

The easiest way is to use the installer:

```bash
./install.sh
# Choose option 4) Install systemd service only
```

This automatically:
1. Creates `/opt/squeeze2diretta/` directory
2. Copies binaries (squeeze2diretta, squeezelite)
3. Installs configuration file and wrapper script
4. Installs and enables the systemd service

## Manual Setup

If you prefer manual installation:

### 1. Create Installation Directory

```bash
sudo mkdir -p /opt/squeeze2diretta
```

### 2. Copy Binaries

```bash
sudo cp build/squeeze2diretta /opt/squeeze2diretta/
sudo cp squeezelite/squeezelite /opt/squeeze2diretta/
sudo chmod +x /opt/squeeze2diretta/squeeze2diretta
sudo chmod +x /opt/squeeze2diretta/squeezelite
```

### 3. Copy Configuration Files

```bash
sudo cp systemd/squeeze2diretta.conf /opt/squeeze2diretta/
sudo cp systemd/start-squeeze2diretta.sh /opt/squeeze2diretta/
sudo chmod +x /opt/squeeze2diretta/start-squeeze2diretta.sh
```

### 4. Edit Configuration

```bash
sudo nano /opt/squeeze2diretta/squeeze2diretta.conf
```

**Required settings:**
- `LMS_SERVER` → Your LMS server IP address (e.g., `192.168.1.104`)
- `TARGET` → Your Diretta target number (run `--list-targets` to find it)

**Optional settings:**
- `PLAYER_NAME` → Name shown in LMS (default: squeeze2diretta)
- `MAX_SAMPLE_RATE` → Maximum sample rate (default: 768000)
- `DSD_FORMAT` → DSD output format: `u32be`, `u32le`, or `dop` (default: u32be)
- `VERBOSE` → Set to `-v` for debug output

### 5. Find Your Diretta Target

```bash
/opt/squeeze2diretta/squeeze2diretta --list-targets
```

### 6. Install Service

```bash
sudo cp systemd/squeeze2diretta.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable squeeze2diretta
```

### 7. Start Service

```bash
sudo systemctl start squeeze2diretta
```

## Service Management

### Start/Stop/Restart

```bash
sudo systemctl start squeeze2diretta
sudo systemctl stop squeeze2diretta
sudo systemctl restart squeeze2diretta
```

### Enable/Disable Auto-Start

```bash
sudo systemctl enable squeeze2diretta   # Start on boot
sudo systemctl disable squeeze2diretta  # Don't start on boot
systemctl is-enabled squeeze2diretta    # Check status
```

### View Logs

```bash
# Real-time logs
sudo journalctl -u squeeze2diretta -f

# Last 50 lines
sudo journalctl -u squeeze2diretta -n 50

# Logs since boot
sudo journalctl -u squeeze2diretta -b

# Logs for specific time
sudo journalctl -u squeeze2diretta --since "1 hour ago"
```

### Check Status

```bash
sudo systemctl status squeeze2diretta
```

## Configuration Reference

The configuration file `/opt/squeeze2diretta/squeeze2diretta.conf`:

```bash
# REQUIRED
LMS_SERVER=192.168.1.100    # LMS server IP address
TARGET=1                     # Diretta target number

# OPTIONAL
PLAYER_NAME=squeeze2diretta  # Player name in LMS
MAX_SAMPLE_RATE=768000       # Max sample rate (Hz)
DSD_FORMAT=u32be             # DSD format (u32be, u32le, dop)
VERBOSE=""                   # Set to "-v" for debug
EXTRA_OPTS=""                # Additional options
```

After modifying, restart the service:
```bash
sudo systemctl restart squeeze2diretta
```

## Performance Tuning

The service file includes performance optimizations:

- **Nice=-10**: Higher CPU priority
- **IOSchedulingClass=realtime**: Real-time I/O scheduling

### Real-Time Priority (Advanced)

To enable real-time scheduling, uncomment these lines in the service file:

```ini
LimitRTPRIO=95
LimitMEMLOCK=infinity
```

Then configure RT limits:

```bash
# Create RT limits configuration
sudo nano /etc/security/limits.d/audio.conf
```

Add:
```
@audio   -  rtprio     95
@audio   -  memlock    unlimited
```

Reboot for changes to take effect.

## Troubleshooting

### Service Won't Start

```bash
# Check syntax errors
sudo systemd-analyze verify squeeze2diretta.service

# View detailed status
sudo systemctl status squeeze2diretta -l

# Check logs for errors
sudo journalctl -u squeeze2diretta -n 50
```

### Common Issues

**Issue: LMS server not found**
- Verify LMS_SERVER IP is correct in config
- Check network connectivity: `ping LMS_SERVER_IP`
- Check firewall allows port 3483 (SlimProto)

**Issue: Diretta target not found**
- Run `/opt/squeeze2diretta/squeeze2diretta --list-targets`
- Check network connectivity to Diretta target
- Verify Diretta target is powered on

**Issue: DSD plays as noise**
- Try different `DSD_FORMAT` values: `u32be`, `u32le`, or `dop`
- For Roon, use `DSD_FORMAT=dop`
- Check DAC supports native DSD

### Test Before Installing

Test the wrapper script manually:

```bash
# Source the config
source /opt/squeeze2diretta/squeeze2diretta.conf

# Run the wrapper
/opt/squeeze2diretta/start-squeeze2diretta.sh
```

## Multiple Instances

To run multiple squeeze2diretta instances (e.g., for different zones):

1. Create separate config files:
```bash
sudo cp /opt/squeeze2diretta/squeeze2diretta.conf /opt/squeeze2diretta/zone2.conf
```

2. Edit zone2.conf with different settings:
```bash
PLAYER_NAME=zone2
TARGET=2
```

3. Create a new service file:
```bash
sudo cp /etc/systemd/system/squeeze2diretta.service /etc/systemd/system/squeeze2diretta-zone2.service
```

4. Edit to use zone2.conf:
```ini
EnvironmentFile=-/opt/squeeze2diretta/zone2.conf
```

5. Enable and start:
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now squeeze2diretta-zone2
```

---

**For more information, see the main [README.md](../README.md)**
