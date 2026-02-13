#!/bin/bash
# Setup script for squeezelite with v2.0 format header patch
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
                libsoxr-dev libssl-dev git patch
            ;;
        fedora|rhel|centos)
            $SUDO dnf install -y gcc make alsa-lib-devel flac-devel \
                libvorbis-devel libmad-devel mpg123-devel opus-devel \
                soxr-devel openssl-devel git patch
            ;;
        arch|manjaro)
            $SUDO pacman -S --needed base-devel alsa-lib flac libvorbis \
                libmad mpg123 opus soxr openssl git patch
            ;;
        *)
            echo -e "${RED}Unsupported OS: $OS${NC}"
            echo "Please install build dependencies manually:"
            echo "  - build-essential / gcc make"
            echo "  - alsa-lib-devel"
            echo "  - libflac-dev, libvorbis-dev, libmad-dev, libmpg123-dev"
            echo "  - libopus-dev, libsoxr-dev, libssl-dev"
            echo "  - patch"
            exit 1
            ;;
    esac

    echo -e "${GREEN}Dependencies installed successfully${NC}"
}

# Check if dependencies are installed
check_dependencies() {
    local missing=0

    for cmd in gcc make git patch; do
        if ! command -v $cmd &> /dev/null; then
            echo -e "${YELLOW}Missing: $cmd${NC}"
            missing=1
        fi
    done

    return $missing
}

# Manual patching fallback: apply patch using git apply
apply_manual_patch() {
    local patch_file="$1"
    echo -e "${GREEN}Trying git apply...${NC}"

    if git apply "$patch_file" 2>/dev/null; then
        echo -e "${GREEN}Patch applied successfully (git apply)${NC}"
    elif git apply --3way "$patch_file" 2>/dev/null; then
        echo -e "${GREEN}Patch applied successfully (git apply --3way)${NC}"
    else
        echo -e "${RED}Failed to apply patch${NC}"
        echo "The patch may not be compatible with this version of squeezelite."
        echo "Try removing the squeezelite directory and re-running this script:"
        echo "  rm -rf $SCRIPT_DIR/squeezelite"
        echo "  ./setup-squeezelite.sh"
        exit 1
    fi
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

    # FIX: Better handling of existing squeezelite directory
    if [ -d "squeezelite" ]; then
        # Check if it's a valid git repo with the required source file
        if [ -d "squeezelite/.git" ] && [ -f "squeezelite/output_stdout.c" ]; then
            echo -e "${YELLOW}squeezelite directory already exists${NC}"
            read -p "Reset to clean state and rebuild? (y/n) " -n 1 -r
            echo
            if [[ $REPLY =~ ^[Yy]$ ]]; then
                echo -e "${GREEN}Resetting squeezelite to clean state...${NC}"
                cd squeezelite
                git checkout -- .
                git clean -fd
                cd "$SCRIPT_DIR"
            else
                echo -e "${YELLOW}Keeping existing directory${NC}"
            fi
        else
            # Directory exists but is incomplete/corrupt
            echo -e "${YELLOW}squeezelite directory exists but appears incomplete${NC}"
            read -p "Remove and re-clone? (y/n) " -n 1 -r
            echo
            if [[ $REPLY =~ ^[Yy]$ ]]; then
                echo -e "${GREEN}Removing incomplete directory...${NC}"
                rm -rf squeezelite
                echo -e "${GREEN}Cloning squeezelite repository...${NC}"
                git clone https://github.com/ralph-irving/squeezelite.git
            else
                echo -e "${RED}Cannot proceed with incomplete squeezelite directory${NC}"
                echo "Please manually remove: $SCRIPT_DIR/squeezelite"
                exit 1
            fi
        fi
    else
        echo -e "${GREEN}Cloning squeezelite repository...${NC}"
        git clone https://github.com/ralph-irving/squeezelite.git
    fi

    cd squeezelite

    # FIX: Verify output_stdout.c exists
    if [ ! -f "output_stdout.c" ]; then
        echo -e "${RED}ERROR: output_stdout.c not found!${NC}"
        echo "The squeezelite repository may not have cloned correctly."
        echo "Please remove the squeezelite directory and try again:"
        echo "  rm -rf $SCRIPT_DIR/squeezelite"
        exit 1
    fi

    # Check if already patched (v2.0 format header patch)
    if grep -q "sq_format_header" output_stdout.c 2>/dev/null; then
        echo -e "${GREEN}v2.0 format header patch already applied${NC}"
    else
        echo -e "${GREEN}Applying v2.0 format header patch...${NC}"

        # Check multiple possible patch file locations
        PATCH_FILE=""
        for path in \
            "$SCRIPT_DIR/patches/squeezelite-format-header.patch" \
            "$SCRIPT_DIR/squeezelite-format-header.patch" \
            "../patches/squeezelite-format-header.patch" \
            "../squeezelite-format-header.patch"; do
            if [ -f "$path" ]; then
                PATCH_FILE="$path"
                break
            fi
        done

        if [ -n "$PATCH_FILE" ]; then
            echo -e "${GREEN}Found patch file: $PATCH_FILE${NC}"

            # Test if patch can be applied before actually applying
            if patch --dry-run -p1 < "$PATCH_FILE" >/dev/null 2>&1; then
                if patch -p1 < "$PATCH_FILE"; then
                    echo -e "${GREEN}Patch applied successfully${NC}"
                else
                    echo -e "${RED}Patch application failed${NC}"
                    exit 1
                fi
            else
                echo -e "${YELLOW}Patch file format doesn't match, trying alternative methods...${NC}"
                apply_manual_patch "$PATCH_FILE"
            fi
        else
            echo -e "${RED}No patch file found${NC}"
            echo "Expected: $SCRIPT_DIR/squeezelite-format-header.patch"
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
    echo "  1. Configure your LMS server and Diretta target"
    echo "  2. Run: ./build/squeeze2diretta --help"
    echo "  3. Start streaming: sudo ./build/squeeze2diretta -s <LMS_IP> -t <target index>"
    echo ""
}

# Run main function
main "$@"
