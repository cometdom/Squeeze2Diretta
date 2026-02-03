#!/bin/bash
#
# squeeze2diretta - Installation Script
#
# This script helps install dependencies and set up squeeze2diretta.
# Run with: bash install.sh
#

set -e  # Exit on error

# =============================================================================
# CONFIGURATION
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="1.0.0"

# Auto-detect latest Diretta SDK version
detect_latest_sdk() {
    local sdk_found=$(find "$HOME" . .. /opt "$HOME/audio" /usr/local \
        -maxdepth 1 -type d -name 'DirettaHostSDK_*' 2>/dev/null | sort -V | tail -1)

    if [ -n "$sdk_found" ]; then
        echo "$sdk_found"
    else
        echo "$HOME/DirettaHostSDK"
    fi
}

SDK_PATH="${DIRETTA_SDK_PATH:-$(detect_latest_sdk)}"

# =============================================================================
# HELPER FUNCTIONS
# =============================================================================

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

print_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error()   { echo -e "${RED}[ERROR]${NC} $1"; }
print_header()  { echo -e "\n${CYAN}=== $1 ===${NC}\n"; }

confirm() {
    local prompt="$1"
    local default="${2:-N}"
    local response

    if [[ "$default" =~ ^[Yy]$ ]]; then
        read -p "$prompt [Y/n]: " response
        response=${response:-Y}
    else
        read -p "$prompt [y/N]: " response
        response=${response:-N}
    fi

    [[ "$response" =~ ^[Yy]$ ]]
}

# =============================================================================
# SYSTEM DETECTION
# =============================================================================

detect_system() {
    print_header "System Detection"

    if [ "$EUID" -eq 0 ]; then
        print_error "Please do not run this script as root"
        print_info "The script will ask for sudo password when needed"
        exit 1
    fi

    if [ -f /etc/os-release ]; then
        . /etc/os-release
        OS=$ID
        VER=$VERSION_ID
        print_success "Detected: $PRETTY_NAME"
    else
        print_error "Cannot detect Linux distribution"
        exit 1
    fi

    # Detect architecture
    ARCH=$(uname -m)
    print_info "Architecture: $ARCH"
}

# =============================================================================
# BASE DEPENDENCIES
# =============================================================================

install_base_dependencies() {
    print_header "Installing Base Dependencies"

    case $OS in
        fedora|rhel|centos)
            print_info "Using DNF package manager..."
            sudo dnf install -y \
                gcc-c++ \
                make \
                cmake \
                git \
                patch \
                wget \
                pkg-config
            ;;
        ubuntu|debian)
            print_info "Using APT package manager..."
            sudo apt update
            sudo apt install -y \
                build-essential \
                cmake \
                git \
                patch \
                wget \
                pkg-config
            ;;
        arch|archarm|manjaro)
            print_info "Using Pacman package manager..."
            sudo pacman -Sy --needed --noconfirm \
                base-devel \
                cmake \
                git \
                patch \
                wget \
                pkgconf
            ;;
        *)
            print_error "Unsupported distribution: $OS"
            print_info "Please install dependencies manually:"
            print_info "  - gcc/g++ (C++ compiler)"
            print_info "  - cmake"
            print_info "  - make"
            print_info "  - git"
            print_info "  - patch"
            exit 1
            ;;
    esac

    print_success "Base dependencies installed"
}

# =============================================================================
# SQUEEZELITE DEPENDENCIES
# =============================================================================

install_squeezelite_dependencies() {
    print_header "Installing Squeezelite Dependencies"

    case $OS in
        fedora|rhel|centos)
            print_info "Installing audio libraries for Squeezelite..."
            sudo dnf install -y \
                alsa-lib-devel \
                flac-devel \
                libvorbis-devel \
                libmad-devel \
                mpg123-devel \
                opus-devel \
                soxr-devel \
                openssl-devel \
                faad2-devel \
                libogg-devel
            ;;
        ubuntu|debian)
            print_info "Installing audio libraries for Squeezelite..."
            sudo apt install -y \
                libasound2-dev \
                libflac-dev \
                libvorbis-dev \
                libmad0-dev \
                libmpg123-dev \
                libopus-dev \
                libsoxr-dev \
                libssl-dev \
                libfaad-dev \
                libogg-dev
            ;;
        arch|archarm|manjaro)
            print_info "Installing audio libraries for Squeezelite..."
            sudo pacman -Sy --needed --noconfirm \
                alsa-lib \
                flac \
                libvorbis \
                libmad \
                mpg123 \
                opus \
                libsoxr \
                openssl \
                faad2 \
                libogg
            ;;
        *)
            print_warning "Cannot auto-install Squeezelite dependencies"
            print_info "Please install audio libraries manually"
            ;;
    esac

    print_success "Squeezelite dependencies installed"
}

# =============================================================================
# SQUEEZELITE SETUP
# =============================================================================

setup_squeezelite() {
    print_header "Setting Up Squeezelite"

    local SQUEEZELITE_DIR="$SCRIPT_DIR/squeezelite"

    # Check if already built
    if [ -f "$SQUEEZELITE_DIR/squeezelite" ]; then
        print_success "Squeezelite binary already exists: $SQUEEZELITE_DIR/squeezelite"
        if ! confirm "Rebuild Squeezelite?"; then
            return 0
        fi
    fi

    # Install dependencies first
    install_squeezelite_dependencies

    # Check for setup script
    if [ -f "$SCRIPT_DIR/setup-squeezelite.sh" ]; then
        print_info "Running setup-squeezelite.sh..."
        chmod +x "$SCRIPT_DIR/setup-squeezelite.sh"
        "$SCRIPT_DIR/setup-squeezelite.sh"
    else
        # Manual setup if script not found
        print_info "setup-squeezelite.sh not found, setting up manually..."

        if [ ! -d "$SQUEEZELITE_DIR" ]; then
            print_info "Cloning Squeezelite repository..."
            git clone https://github.com/ralph-irving/squeezelite.git "$SQUEEZELITE_DIR"
        else
            print_info "Updating Squeezelite repository..."
            cd "$SQUEEZELITE_DIR"
            git pull
        fi

        cd "$SQUEEZELITE_DIR"

        # Apply stdout flush patch if exists
        if [ -f "$SCRIPT_DIR/patches/squeezelite-stdout-flush.patch" ]; then
            print_info "Applying stdout flush patch..."
            git checkout . 2>/dev/null || true
            patch -p1 < "$SCRIPT_DIR/patches/squeezelite-stdout-flush.patch" || print_warning "Patch may have already been applied"
        fi

        # Build with DSD support
        print_info "Building Squeezelite with DSD support..."
        make clean 2>/dev/null || true
        OPTS="-DDSD -DRESAMPLE" make -j$(nproc)

        cd "$SCRIPT_DIR"
    fi

    # Verify build
    if [ -f "$SQUEEZELITE_DIR/squeezelite" ]; then
        print_success "Squeezelite built successfully!"
        "$SQUEEZELITE_DIR/squeezelite" -? 2>&1 | head -3 || true
    else
        print_error "Squeezelite build failed"
        exit 1
    fi
}

# =============================================================================
# DIRETTA SDK
# =============================================================================

check_diretta_sdk() {
    print_header "Diretta SDK Check"

    # Auto-detect all DirettaHostSDK_* directories
    local sdk_candidates=()
    while IFS= read -r sdk_dir; do
        sdk_candidates+=("$sdk_dir")
    done < <(find "$HOME" . .. /opt "$HOME/audio" /usr/local \
        -maxdepth 1 -type d -name 'DirettaHostSDK_*' 2>/dev/null | sort -Vr)

    # Also add SDK_PATH if set
    [ -d "$SDK_PATH" ] && sdk_candidates=("$SDK_PATH" "${sdk_candidates[@]}")

    # Try each candidate
    for loc in "${sdk_candidates[@]}"; do
        if [ -d "$loc" ] && [ -d "$loc/lib" ]; then
            SDK_PATH="$loc"
            local sdk_version=$(basename "$loc" | sed 's/DirettaHostSDK_//')
            print_success "Found Diretta SDK at: $SDK_PATH"
            [ -n "$sdk_version" ] && print_info "SDK version: $sdk_version"
            return 0
        fi
    done

    print_warning "Diretta SDK not found"
    echo ""
    echo "The Diretta Host SDK is required but not included in this repository."
    echo ""
    echo "Please download it from: https://www.diretta.link/hostsdk.html"
    echo "  1. Visit the website"
    echo "  2. Download DirettaHostSDK_XXX.tar.gz (latest version)"
    echo "  3. Extract to: $HOME/"
    echo ""
    read -p "Press Enter after you've downloaded and extracted the SDK..."

    # Check again after user extraction
    while IFS= read -r sdk_dir; do
        if [ -d "$sdk_dir" ] && [ -d "$sdk_dir/lib" ]; then
            SDK_PATH="$sdk_dir"
            print_success "Found Diretta SDK at: $SDK_PATH"
            return 0
        fi
    done < <(find "$HOME" . .. /opt -maxdepth 1 -type d -name 'DirettaHostSDK_*' 2>/dev/null | sort -Vr)

    print_error "SDK still not found. Please extract it and try again."
    exit 1
}

# =============================================================================
# BUILD SQUEEZE2DIRETTA
# =============================================================================

build_squeeze2diretta() {
    print_header "Building squeeze2diretta"

    cd "$SCRIPT_DIR"

    # Clean and create build directory (ensure fresh compilation)
    if [ -d "build" ]; then
        print_info "Cleaning previous build..."
        rm -rf build
    fi
    mkdir -p build
    cd build

    # Configure with CMake
    print_info "Configuring with CMake..."
    export DIRETTA_SDK_PATH="$SDK_PATH"
    cmake ..

    # Build
    print_info "Building squeeze2diretta..."
    make -j$(nproc)

    # Verify build
    if [ -f "squeeze2diretta" ]; then
        print_success "Build successful!"
        print_info "Binary: $SCRIPT_DIR/build/squeeze2diretta"
    else
        print_error "Build failed. Please check error messages above."
        exit 1
    fi

    cd "$SCRIPT_DIR"
}

# =============================================================================
# NETWORK CONFIGURATION
# =============================================================================

configure_network() {
    print_header "Network Configuration"

    echo "Available network interfaces:"
    ip link show | grep -E "^[0-9]+:" | awk '{print "  " $2}' | sed 's/://g'
    echo ""

    read -p "Enter network interface for Diretta (e.g., eth0) or press Enter to skip: " IFACE

    if [ -z "$IFACE" ]; then
        print_info "Skipping network configuration"
        return 0
    fi

    if ! ip link show "$IFACE" &> /dev/null; then
        print_error "Interface $IFACE not found"
        return 1
    fi

    if confirm "Enable jumbo frames for better DSD performance?"; then
        echo ""
        echo "Select MTU size (must match your Diretta Target setting):"
        echo ""
        echo "  1) MTU 9014  - Standard jumbo frames"
        echo "  2) MTU 16128 - Maximum jumbo frames (recommended)"
        echo "  3) Skip"
        echo ""
        read -rp "Choice [1-3]: " mtu_choice

        local MTU_VALUE=""
        case $mtu_choice in
            1) MTU_VALUE=9014 ;;
            2) MTU_VALUE=16128 ;;
            3|"")
                print_info "Skipping MTU configuration"
                ;;
            *)
                print_warning "Invalid choice, skipping MTU configuration"
                ;;
        esac

        if [ -n "$MTU_VALUE" ]; then
            sudo ip link set "$IFACE" mtu "$MTU_VALUE"
            print_success "Jumbo frames enabled (MTU $MTU_VALUE)"

            if confirm "Make this permanent?"; then
                case $OS in
                    fedora|rhel|centos)
                        local conn_name
                        conn_name=$(nmcli -t -f NAME,DEVICE connection show 2>/dev/null | grep "$IFACE" | cut -d: -f1)
                        if [ -n "$conn_name" ]; then
                            sudo nmcli connection modify "$conn_name" 802-3-ethernet.mtu "$MTU_VALUE"
                            print_success "MTU configured permanently in NetworkManager"
                        else
                            print_warning "Could not find NetworkManager connection for $IFACE"
                        fi
                        ;;
                    ubuntu|debian)
                        print_info "Add 'mtu $MTU_VALUE' to /etc/network/interfaces for $IFACE"
                        ;;
                    *)
                        print_info "Manual configuration required for permanent MTU"
                        ;;
                esac
            fi
        fi
    fi

    # Network buffer optimization
    if confirm "Optimize network buffers for audio streaming (16MB)?"; then
        print_info "Setting network buffer sizes..."
        sudo sysctl -w net.core.rmem_max=16777216
        sudo sysctl -w net.core.wmem_max=16777216
        print_success "Network buffers set to 16MB"

        if confirm "Make this permanent?"; then
            sudo tee /etc/sysctl.d/99-squeeze2diretta.conf > /dev/null <<'SYSCTL'
# squeeze2diretta - Network buffer optimization
# Larger buffers help with high-resolution audio and DSD streaming
net.core.rmem_max=16777216
net.core.wmem_max=16777216
SYSCTL
            sudo sysctl --system > /dev/null
            print_success "Network buffer settings saved to /etc/sysctl.d/99-squeeze2diretta.conf"
        fi
    fi
}

# =============================================================================
# FIREWALL CONFIGURATION
# =============================================================================

configure_firewall() {
    print_header "Firewall Configuration"

    if ! confirm "Configure firewall to allow LMS/Squeezelite traffic?"; then
        print_info "Skipping firewall configuration"
        return 0
    fi

    case $OS in
        fedora|rhel|centos)
            if command -v firewall-cmd &> /dev/null; then
                # SlimProto port
                sudo firewall-cmd --permanent --add-port=3483/tcp
                sudo firewall-cmd --permanent --add-port=3483/udp
                # HTTP streaming
                sudo firewall-cmd --permanent --add-port=9000/tcp
                sudo firewall-cmd --reload
                print_success "Firewall configured (firewalld)"
            else
                print_info "firewalld not installed, skipping"
            fi
            ;;
        ubuntu|debian)
            if command -v ufw &> /dev/null; then
                sudo ufw allow 3483/tcp
                sudo ufw allow 3483/udp
                sudo ufw allow 9000/tcp
                print_success "Firewall configured (ufw)"
            else
                print_info "ufw not installed, skipping"
            fi
            ;;
        *)
            print_info "Manual firewall configuration required"
            print_info "Open ports: 3483/tcp, 3483/udp, 9000/tcp"
            ;;
    esac
}

# =============================================================================
# SYSTEMD SERVICE
# =============================================================================

setup_systemd_service() {
    print_header "Systemd Service Installation"

    local INSTALL_DIR="/opt/squeeze2diretta"
    local SERVICE_FILE="/etc/systemd/system/squeeze2diretta.service"
    local CONFIG_FILE="$INSTALL_DIR/squeeze2diretta.conf"
    local WRAPPER_SCRIPT="$INSTALL_DIR/start-squeeze2diretta.sh"
    local BINARY_PATH="$SCRIPT_DIR/build/squeeze2diretta"
    local SQUEEZELITE_PATH="$SCRIPT_DIR/squeezelite/squeezelite"

    # Check if binary exists
    if [ ! -f "$BINARY_PATH" ]; then
        print_error "Binary not found at: $BINARY_PATH"
        print_info "Please build squeeze2diretta first (option 3)"
        return 1
    fi

    # Check if squeezelite exists
    if [ ! -f "$SQUEEZELITE_PATH" ]; then
        print_error "Squeezelite not found at: $SQUEEZELITE_PATH"
        print_info "Please set up Squeezelite first (option 2)"
        return 1
    fi

    print_success "Binary found: $BINARY_PATH"
    print_success "Squeezelite found: $SQUEEZELITE_PATH"

    if ! confirm "Install systemd service to $INSTALL_DIR?"; then
        print_info "Skipping systemd service setup"
        return 0
    fi

    print_info "1. Creating installation directory..."
    sudo mkdir -p "$INSTALL_DIR"

    print_info "2. Copying binaries..."
    sudo cp "$BINARY_PATH" "$INSTALL_DIR/"
    sudo cp "$SQUEEZELITE_PATH" "$INSTALL_DIR/"
    sudo chmod +x "$INSTALL_DIR/squeeze2diretta"
    sudo chmod +x "$INSTALL_DIR/squeezelite"
    print_success "Binaries copied to $INSTALL_DIR/"

    print_info "3. Creating wrapper script..."
    sudo tee "$WRAPPER_SCRIPT" > /dev/null <<'WRAPPER_EOF'
#!/bin/bash
# squeeze2diretta - Startup Wrapper Script
# This script reads configuration and starts squeeze2diretta with appropriate options

set -e

# Default values (can be overridden by config file)
LMS_SERVER="${LMS_SERVER:-192.168.1.100}"
TARGET="${TARGET:-1}"
PLAYER_NAME="${PLAYER_NAME:-squeeze2diretta}"
MAX_SAMPLE_RATE="${MAX_SAMPLE_RATE:-768000}"
DSD_FORMAT="${DSD_FORMAT:-u32be}"
PAUSE_ON_START="${PAUSE_ON_START:-no}"
VERBOSE="${VERBOSE:-}"
EXTRA_OPTS="${EXTRA_OPTS:-}"

INSTALL_DIR="/opt/squeeze2diretta"
SQUEEZE2DIRETTA="$INSTALL_DIR/squeeze2diretta"
SQUEEZELITE="$INSTALL_DIR/squeezelite"

# Build command
CMD="$SQUEEZE2DIRETTA"
CMD="$CMD --squeezelite $SQUEEZELITE"
CMD="$CMD -s $LMS_SERVER"
CMD="$CMD --target $TARGET"
CMD="$CMD -n $PLAYER_NAME"
CMD="$CMD -r $MAX_SAMPLE_RATE"

# DSD format: u32be (native, default for LMS) or dop (for Roon)
# Note: Add colon prefix for native DSD formats (required by squeezelite)
if [ "$DSD_FORMAT" = "dop" ]; then
    CMD="$CMD -D"
elif [ "$DSD_FORMAT" = "u32be" ] || [ "$DSD_FORMAT" = "u32le" ]; then
    CMD="$CMD -D :$DSD_FORMAT"
else
    CMD="$CMD -D $DSD_FORMAT"
fi

# Optional verbose mode
if [ -n "$VERBOSE" ]; then
    CMD="$CMD -v"
fi

# Extra options
if [ -n "$EXTRA_OPTS" ]; then
    CMD="$CMD $EXTRA_OPTS"
fi

# Log the command being executed
echo "════════════════════════════════════════════════════════"
echo "  Starting squeeze2diretta v1.0.0"
echo "════════════════════════════════════════════════════════"
echo ""
echo "Configuration:"
echo "  LMS Server:       $LMS_SERVER"
echo "  Diretta Target:   $TARGET"
echo "  Player Name:      $PLAYER_NAME"
echo "  Max Sample Rate:  $MAX_SAMPLE_RATE"
echo "  DSD Format:       $DSD_FORMAT"
echo "  Pause on Start:   $PAUSE_ON_START"
echo ""
echo "Command:"
echo "  $CMD"
echo ""
echo "════════════════════════════════════════════════════════"
echo ""

# Function to send pause command to LMS
send_pause_command() {
    # Wait for squeezelite to connect to LMS
    sleep 5
    # URL-encode the player name (replace spaces with %20)
    ENCODED_NAME=$(echo "$PLAYER_NAME" | sed 's/ /%20/g')
    # Send pause command via LMS CLI (port 9090)
    echo "$ENCODED_NAME pause 1" | nc -w 2 "$LMS_SERVER" 9090 > /dev/null 2>&1 || true
    echo "[PAUSE_ON_START] Sent pause command to LMS for player: $PLAYER_NAME"
}

# If PAUSE_ON_START is enabled, run pause command in background
if [ "$PAUSE_ON_START" = "yes" ] || [ "$PAUSE_ON_START" = "true" ] || [ "$PAUSE_ON_START" = "1" ]; then
    echo "[PAUSE_ON_START] Will pause playback after connection..."
    send_pause_command &
fi

# Execute
exec $CMD
WRAPPER_EOF
    sudo chmod +x "$WRAPPER_SCRIPT"
    print_success "Wrapper script created: $WRAPPER_SCRIPT"

    print_info "4. Creating configuration file..."
    if [ ! -f "$CONFIG_FILE" ]; then
        sudo tee "$CONFIG_FILE" > /dev/null <<'CONFIG_EOF'
# squeeze2diretta Configuration
#
# This file is sourced by the systemd service unit.
# Modify these values to change behavior.
#
# After changing values, restart the service:
#   sudo systemctl restart squeeze2diretta

# ============================================================================
# REQUIRED SETTINGS - You MUST configure these
# ============================================================================

# LMS (Lyrion Music Server) IP address
# This is the server running LMS that has your music library
LMS_SERVER=192.168.1.100

# Diretta target number
# Run: /opt/squeeze2diretta/squeeze2diretta --list-targets
# to see available targets and their numbers
TARGET=1

# ============================================================================
# OPTIONAL SETTINGS
# ============================================================================

# Player name (shown in LMS web interface)
PLAYER_NAME=squeeze2diretta

# Maximum sample rate in Hz
# Common values: 192000, 384000, 768000
MAX_SAMPLE_RATE=768000

# DSD output format
# Options:
#   u32be  = Native DSD Big Endian (default, for LMS)
#   u32le  = Native DSD Little Endian
#   dop    = DSD over PCM (for Roon)
DSD_FORMAT=u32be

# Pause on start
# Set to "yes" to pause playback when the service starts
# This prevents music from auto-resuming after a reboot
# Leave as "no" for normal LMS behavior (resume playback)
PAUSE_ON_START=no

# Verbose mode
# Set to "-v" for debug output, leave empty for normal operation
VERBOSE=""

# Extra options to pass to squeeze2diretta
# Example: EXTRA_OPTS="-d all=info"
EXTRA_OPTS=""

# ============================================================================
# TROUBLESHOOTING
# ============================================================================
#
# Problem: No sound
# Solution: Check LMS_SERVER IP is correct and reachable
#           Check TARGET number matches your Diretta device
#
# Problem: DSD files play as noise
# Solution: Check that your DAC supports native DSD
#           Try different DSD_FORMAT values (u32be, u32le, dop)
#
# Problem: Using Roon instead of LMS
# Solution: Set DSD_FORMAT=dop (Roon sends DoP, not native DSD)
#
# Problem: Player not showing in LMS
# Solution: Check firewall allows ports 3483 and 9000
#           Verify LMS server is running
#
# For more help: https://github.com/cometdom/squeeze2diretta
CONFIG_EOF
        print_success "Configuration file created: $CONFIG_FILE"
    else
        print_info "Configuration file already exists, keeping current settings"
    fi

    print_info "5. Installing systemd service..."
    sudo tee "$SERVICE_FILE" > /dev/null <<'SERVICE_EOF'
[Unit]
Description=squeeze2diretta - Squeezelite to Diretta Bridge
Documentation=https://github.com/cometdom/squeeze2diretta
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/squeeze2diretta
EnvironmentFile=-/opt/squeeze2diretta/squeeze2diretta.conf

# Use wrapper script to handle configuration
ExecStart=/opt/squeeze2diretta/start-squeeze2diretta.sh

Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal
SyslogIdentifier=squeeze2diretta

# Performance optimizations
Nice=-10
IOSchedulingClass=realtime
IOSchedulingPriority=0

[Install]
WantedBy=multi-user.target
SERVICE_EOF
    print_success "Service file installed: $SERVICE_FILE"

    print_info "6. Reloading systemd daemon..."
    sudo systemctl daemon-reload

    print_info "7. Enabling service (start on boot)..."
    sudo systemctl enable squeeze2diretta.service

    echo ""
    print_success "Systemd Service Installation Complete!"
    echo ""
    echo "  Configuration: $CONFIG_FILE"
    echo "  Service file:  $SERVICE_FILE"
    echo "  Install dir:   $INSTALL_DIR"
    echo ""
    echo "  IMPORTANT: Edit the configuration file before starting!"
    echo ""
    echo "  Next steps:"
    echo "    1. Edit configuration (REQUIRED):"
    echo "       sudo nano $CONFIG_FILE"
    echo "       - Set LMS_SERVER to your LMS IP address"
    echo "       - Set TARGET to your Diretta device number"
    echo ""
    echo "    2. Find your Diretta target number:"
    echo "       $INSTALL_DIR/squeeze2diretta --list-targets"
    echo ""
    echo "    3. Start the service:"
    echo "       sudo systemctl start squeeze2diretta"
    echo ""
    echo "    4. Check status:"
    echo "       sudo systemctl status squeeze2diretta"
    echo ""
    echo "    5. View logs:"
    echo "       sudo journalctl -u squeeze2diretta -f"
    echo ""
}

# =============================================================================
# FEDORA AGGRESSIVE OPTIMIZATION (OPTIONAL)
# =============================================================================

optimize_fedora_aggressive() {
    print_header "Aggressive Fedora Optimization"

    if [ "$OS" != "fedora" ]; then
        print_warning "This optimization is only for Fedora systems"
        return 1
    fi

    echo ""
    echo "WARNING: This will make aggressive changes to your system:"
    echo ""
    echo "  - Remove firewalld (firewall disabled)"
    echo "  - Remove SELinux policy (security framework disabled)"
    echo "  - Disable systemd-journald (no persistent logs)"
    echo "  - Disable systemd-oomd (out-of-memory daemon)"
    echo "  - Disable systemd-homed (home directory manager)"
    echo "  - Disable auditd (audit daemon)"
    echo "  - Remove polkit (privilege manager)"
    echo "  - Replace sshd with dropbear (lightweight SSH)"
    echo ""
    echo "This is intended for DEDICATED AUDIO SERVERS ONLY."
    echo "Do NOT use on general-purpose systems or servers with"
    echo "sensitive data."
    echo ""

    if ! confirm "Are you sure you want to proceed with aggressive optimization?" "N"; then
        print_info "Optimization cancelled"
        return 0
    fi

    echo ""
    if ! confirm "FINAL WARNING: This will significantly reduce system security. Continue?" "N"; then
        print_info "Optimization cancelled"
        return 0
    fi

    print_info "Starting aggressive optimization..."

    # Install kernel development tools
    print_info "Installing development tools..."
    sudo dnf install -y kernel-devel make dwarves tar zstd rsync curl which || true
    sudo dnf install -y gcc bc bison flex perl elfutils-libelf-devel elfutils-devel openssl openssl-devel rpm-build ncurses-devel || true

    # Disable and remove security services
    print_info "Disabling security services..."

    sudo systemctl disable auditd 2>/dev/null || true
    sudo systemctl stop auditd 2>/dev/null || true

    sudo systemctl stop firewalld 2>/dev/null || true
    sudo systemctl disable firewalld 2>/dev/null || true
    sudo dnf remove -y firewalld 2>/dev/null || true

    sudo dnf remove -y selinux-policy 2>/dev/null || true

    # Disable system services that add overhead
    print_info "Disabling system overhead services..."

    sudo systemctl disable systemd-journald 2>/dev/null || true
    sudo systemctl stop systemd-journald 2>/dev/null || true

    sudo systemctl disable systemd-oomd 2>/dev/null || true
    sudo systemctl stop systemd-oomd 2>/dev/null || true

    sudo systemctl disable systemd-homed 2>/dev/null || true
    sudo systemctl stop systemd-homed 2>/dev/null || true

    sudo systemctl stop polkitd 2>/dev/null || true
    sudo dnf remove -y polkit 2>/dev/null || true

    sudo dnf remove -y gssproxy 2>/dev/null || true

    # Replace sshd with dropbear
    print_info "Installing lightweight SSH server (dropbear)..."
    sudo dnf install -y dropbear || {
        print_warning "Failed to install dropbear, keeping sshd"
    }

    if command -v dropbear &> /dev/null; then
        sudo systemctl enable dropbear || true
        sudo systemctl start dropbear || true

        sudo systemctl disable sshd 2>/dev/null || true
        sudo systemctl stop sshd 2>/dev/null || true

        print_success "Dropbear installed and running"
    fi

    # Network buffer optimization
    print_info "Optimizing network buffers..."
    sudo sysctl -w net.core.rmem_max=16777216
    sudo sysctl -w net.core.wmem_max=16777216
    sudo tee /etc/sysctl.d/99-squeeze2diretta.conf > /dev/null <<'SYSCTL'
# squeeze2diretta - Network buffer optimization
# Larger buffers help with high-resolution audio and DSD streaming
net.core.rmem_max=16777216
net.core.wmem_max=16777216
SYSCTL
    sudo sysctl --system > /dev/null
    print_success "Network buffers optimized (16MB)"

    # Install useful tools
    sudo dnf install -y htop || true

    print_success "Aggressive optimization complete"
    print_warning "A reboot is recommended to apply all changes"

    if confirm "Reboot now?"; then
        sudo reboot
    fi
}

# =============================================================================
# TEST INSTALLATION
# =============================================================================

test_installation() {
    print_header "Testing Installation"

    local BINARY="$SCRIPT_DIR/build/squeeze2diretta"
    local SQUEEZELITE="$SCRIPT_DIR/squeezelite/squeezelite"

    # Check squeeze2diretta binary
    if [ -f "$BINARY" ]; then
        print_success "squeeze2diretta binary: OK"
    else
        print_error "squeeze2diretta binary: NOT FOUND"
        return 1
    fi

    # Check squeezelite binary
    if [ -f "$SQUEEZELITE" ]; then
        print_success "squeezelite binary: OK"
        echo ""
        print_info "Squeezelite version:"
        "$SQUEEZELITE" -? 2>&1 | head -3 || true
    else
        print_error "squeezelite binary: NOT FOUND"
        return 1
    fi

    # List Diretta targets (with timeout to avoid blocking)
    echo ""
    print_info "Searching for Diretta targets..."
    print_info "(Press Ctrl+C if it takes too long)"
    timeout 10 "$BINARY" --list-targets 2>&1 || {
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            print_info "Target search timed out (this is normal if no targets found)"
        else
            print_warning "Could not list Diretta targets"
            print_info "Make sure a Diretta device is connected to your network"
        fi
    }

    echo ""
    print_success "Installation test complete!"
}

# =============================================================================
# MAIN MENU
# =============================================================================

show_main_menu() {
    echo ""
    echo "============================================"
    echo " squeeze2diretta v$VERSION - Installation"
    echo "============================================"
    echo ""
    echo "Installation options:"
    echo ""
    echo "  1) Full installation (recommended)"
    echo "     - Dependencies, Squeezelite, build, systemd service"
    echo ""
    echo "  2) Setup Squeezelite only"
    echo "     - Download, patch, and compile Squeezelite"
    echo ""
    echo "  3) Build squeeze2diretta only"
    echo "     - Compile squeeze2diretta (assumes dependencies installed)"
    echo ""
    echo "  4) Install systemd service only"
    echo "     - Install as system service (assumes built)"
    echo ""
    echo "  5) Configure network only"
    echo "     - Network interface, MTU, and firewall setup"
    echo ""
    echo "  6) Test installation"
    echo "     - Verify binaries and list Diretta targets"
    echo ""
    if [ "$OS" = "fedora" ]; then
    echo "  7) Aggressive Fedora optimization"
    echo "     - For dedicated audio servers only"
    echo ""
    fi
    echo "  q) Quit"
    echo ""
}

run_full_installation() {
    install_base_dependencies
    setup_squeezelite
    check_diretta_sdk
    build_squeeze2diretta
    configure_network
    configure_firewall
    setup_systemd_service
    test_installation

    print_header "Installation Complete!"

    echo ""
    echo "Quick Start:"
    echo ""
    echo "  1. Edit configuration (REQUIRED):"
    echo "     sudo nano /opt/squeeze2diretta/squeeze2diretta.conf"
    echo "     - Set LMS_SERVER to your LMS IP address"
    echo "     - Set TARGET to your Diretta device number"
    echo ""
    echo "  2. Find Diretta targets:"
    echo "     /opt/squeeze2diretta/squeeze2diretta --list-targets"
    echo ""
    echo "  3. Start the service:"
    echo "     sudo systemctl start squeeze2diretta"
    echo ""
    echo "  4. Check status:"
    echo "     sudo systemctl status squeeze2diretta"
    echo ""
    echo "  5. View logs:"
    echo "     sudo journalctl -u squeeze2diretta -f"
    echo ""
    echo "  6. Open LMS web interface and select 'squeeze2diretta' as player"
    echo ""
    echo "Documentation:"
    echo "  - README.md - Overview and quick start"
    echo "  - systemd/README.md - Service configuration"
    echo ""
}

# =============================================================================
# ENTRY POINT
# =============================================================================

main() {
    detect_system

    # Check for command-line arguments
    case "${1:-}" in
        --full|-f)
            run_full_installation
            exit 0
            ;;
        --squeezelite|-sq)
            install_base_dependencies
            setup_squeezelite
            exit 0
            ;;
        --build|-b)
            check_diretta_sdk
            build_squeeze2diretta
            exit 0
            ;;
        --service|-s)
            setup_systemd_service
            exit 0
            ;;
        --network|-n)
            configure_network
            configure_firewall
            exit 0
            ;;
        --test|-t)
            test_installation
            exit 0
            ;;
        --optimize|-o)
            optimize_fedora_aggressive
            exit 0
            ;;
        --help|-h)
            echo "Usage: $0 [OPTION]"
            echo ""
            echo "Options:"
            echo "  --full, -f          Full installation"
            echo "  --squeezelite, -sq  Setup Squeezelite only"
            echo "  --build, -b         Build squeeze2diretta only"
            echo "  --service, -s       Install systemd service only"
            echo "  --network, -n       Configure network only"
            echo "  --test, -t          Test installation"
            echo "  --optimize, -o      Aggressive Fedora optimization"
            echo "  --help, -h          Show this help"
            echo ""
            echo "Without options, shows interactive menu."
            exit 0
            ;;
    esac

    # Interactive menu
    while true; do
        show_main_menu

        local max_option=6
        [ "$OS" = "fedora" ] && max_option=7

        read -p "Choose option [1-$max_option/q]: " choice

        case $choice in
            1)
                run_full_installation
                break
                ;;
            2)
                install_base_dependencies
                setup_squeezelite
                print_success "Squeezelite setup complete"
                ;;
            3)
                check_diretta_sdk
                build_squeeze2diretta
                ;;
            4)
                setup_systemd_service
                ;;
            5)
                configure_network
                configure_firewall
                print_success "Network configuration complete"
                ;;
            6)
                test_installation
                ;;
            7)
                if [ "$OS" = "fedora" ]; then
                    optimize_fedora_aggressive
                else
                    print_error "Invalid option"
                fi
                ;;
            q|Q)
                print_info "Exiting..."
                exit 0
                ;;
            *)
                print_error "Invalid option: $choice"
                ;;
        esac
    done
}

# Run main
main "$@"
