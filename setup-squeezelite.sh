#!/bin/bash
# Setup script for squeezelite with stdout flush patch
# This script clones, patches, compiles and installs squeezelite for squeeze2diretta

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "================================================================"
echo "  Squeezelite Setup for squeeze2diretta"
echo "  This will build and install a patched version of squeezelite"
echo "================================================================"
echo ""

# Detect OS and package manager
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
else
    echo -e "${RED}Cannot detect OS${NC}"
    exit 1
fi

# Check if running as root for package installation
if [ "$EUID" -eq 0 ]; then
    echo -e "${YELLOW}Warning: Running as root. This is not recommended.${NC}"
    SUDO=""
else
    SUDO="sudo"
fi

# Function to install dependencies
install_dependencies() {
    echo -e "${GREEN}Installing build dependencies...${NC}"

    case $OS in
        ubuntu|debian)
            $SUDO apt update
            $SUDO apt install -y build-essential libasound2-dev libflac-dev \
                libvorbis-dev libmad0-dev libmpg123-dev libopus-dev \
                libsoxr-dev libssl-dev git
            ;;
        fedora|rhel|centos)
            $SUDO dnf install -y gcc make alsa-lib-devel flac-devel \
                libvorbis-devel libmad-devel mpg123-devel opus-devel \
                soxr-devel openssl-devel git
            ;;
        arch|manjaro)
            $SUDO pacman -S --needed base-devel alsa-lib flac libvorbis \
                libmad mpg123 opus soxr openssl git
            ;;
        *)
            echo -e "${RED}Unsupported OS: $OS${NC}"
            echo "Please install build dependencies manually:"
            echo "  - build-essential / gcc make"
            echo "  - alsa-lib-devel"
            echo "  - libflac-dev, libvorbis-dev, libmad-dev, libmpg123-dev"
            echo "  - libopus-dev, libsoxr-dev, libssl-dev"
            exit 1
            ;;
    esac

    echo -e "${GREEN}Dependencies installed successfully${NC}"
}

# Check if dependencies are installed
check_dependencies() {
    local missing=0

    for cmd in gcc make git; do
        if ! command -v $cmd &> /dev/null; then
            echo -e "${YELLOW}Missing: $cmd${NC}"
            missing=1
        fi
    done

    return $missing
}

# Main installation
main() {
    local SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
    cd "$SCRIPT_DIR"

    # Check/install dependencies
    if ! check_dependencies; then
        echo -e "${YELLOW}Some dependencies are missing${NC}"
        read -p "Install dependencies automatically? (y/n) " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            install_dependencies
        else
            echo -e "${RED}Please install dependencies manually${NC}"
            exit 1
        fi
    else
        echo -e "${GREEN}All dependencies found${NC}"
    fi

    # Clone squeezelite if not exists
    if [ ! -d "squeezelite" ]; then
        echo -e "${GREEN}Cloning squeezelite repository...${NC}"
        git clone https://github.com/ralph-irving/squeezelite.git
    else
        echo -e "${YELLOW}squeezelite directory already exists, skipping clone${NC}"
    fi

    cd squeezelite

    # Check if already patched
    if grep -q "fflush(stdout).*Force flush when stdout is redirected to pipe" output_stdout.c 2>/dev/null; then
        echo -e "${GREEN}Patch already applied${NC}"
    else
        echo -e "${GREEN}Applying stdout flush patch...${NC}"

        # Try to apply patch file
        if [ -f "../squeezelite-stdout-flush.patch" ]; then
            if patch -p1 < ../squeezelite-stdout-flush.patch 2>/dev/null; then
                echo -e "${GREEN}Patch applied successfully${NC}"
            else
                # Apply manually
                echo -e "${YELLOW}Automatic patch failed, applying manually...${NC}"

                # Find the line with fwrite and add fflush after it
                sed -i '/fwrite(buf, bytes_per_frame, buffill, stdout);/a\			fflush(stdout);  \/\/ Force flush when stdout is redirected to pipe' output_stdout.c

                if grep -q "fflush(stdout)" output_stdout.c; then
                    echo -e "${GREEN}Manual patch applied successfully${NC}"
                else
                    echo -e "${RED}Failed to apply patch${NC}"
                    echo "Please manually edit output_stdout.c and add this line after line 116:"
                    echo "    fflush(stdout);  // Force flush when stdout is redirected to pipe"
                    exit 1
                fi
            fi
        else
            echo -e "${RED}Patch file not found${NC}"
            exit 1
        fi
    fi

    # Compile
    echo -e "${GREEN}Compiling squeezelite...${NC}"
    make clean 2>/dev/null || true
    make OPTS="-DDSD -DRESAMPLE -DNO_FAAD" -j$(nproc)

    if [ ! -f "squeezelite" ]; then
        echo -e "${RED}Compilation failed${NC}"
        exit 1
    fi

    echo -e "${GREEN}Compilation successful${NC}"

    # Install
    local INSTALL_PATH="/usr/local/bin/squeezelite"

    read -p "Install squeezelite to $INSTALL_PATH? (y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        $SUDO cp squeezelite "$INSTALL_PATH"
        $SUDO chmod +x "$INSTALL_PATH"
        echo -e "${GREEN}Installed to $INSTALL_PATH${NC}"
    else
        echo -e "${YELLOW}Skipping installation${NC}"
        echo "Binary is at: $(pwd)/squeezelite"
    fi

    # Test
    echo ""
    echo -e "${GREEN}Testing squeezelite...${NC}"
    if ./squeezelite -t 2>&1 | head -5; then
        echo -e "${GREEN}Squeezelite test successful${NC}"
    else
        echo -e "${YELLOW}Test completed (exit code: $?)${NC}"
    fi

    # Build squeeze2diretta
    cd "$SCRIPT_DIR"
    echo ""
    read -p "Build squeeze2diretta wrapper? (y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo -e "${GREEN}Building squeeze2diretta...${NC}"
        mkdir -p build
        cd build
        cmake ..
        make -j$(nproc)

        if [ -f "squeeze2diretta" ]; then
            echo -e "${GREEN}Build successful${NC}"
            echo "Binary: $(pwd)/squeeze2diretta"
        else
            echo -e "${RED}Build failed${NC}"
            exit 1
        fi
    fi

    echo ""
    echo "================================================================"
    echo -e "${GREEN}Setup completed successfully!${NC}"
    echo "================================================================"
    echo ""
    echo "Next steps:"
    echo "  1. List available Diretta targets:"
    echo "     ./build/squeeze2diretta --list-targets"
    echo ""
    echo "  2. Test manually:"
    echo "     ./build/squeeze2diretta --squeezelite ./squeezelite/squeezelite -s <LMS_IP> --target <TARGET_NUM>"
    echo ""
    echo "  3. Or install the systemd service:"
    echo "     ./install.sh  (choose option 4)"
    echo ""
}

# Run main function
main "$@"
