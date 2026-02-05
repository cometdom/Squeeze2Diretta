#!/bin/bash
# squeeze2diretta - Startup Wrapper Script
# This script reads configuration and starts squeeze2diretta with appropriate options

set -e

INSTALL_DIR="/opt/squeeze2diretta"
CONFIG_FILE="$INSTALL_DIR/squeeze2diretta.conf"

# Source config file if it exists
if [ -f "$CONFIG_FILE" ]; then
    # shellcheck source=/dev/null
    source "$CONFIG_FILE"
fi

# Default values (if not set by config file)
LMS_SERVER="${LMS_SERVER:-192.168.1.100}"
TARGET="${TARGET:-1}"
PLAYER_NAME="${PLAYER_NAME:-squeeze2diretta}"
MAX_SAMPLE_RATE="${MAX_SAMPLE_RATE:-768000}"
DSD_FORMAT="${DSD_FORMAT:-u32be}"
PAUSE_ON_START="${PAUSE_ON_START:-no}"
SAMPLE_FORMAT="${SAMPLE_FORMAT:-32}"
WAV_HEADER="${WAV_HEADER:-no}"
VERBOSE="${VERBOSE:-}"
EXTRA_OPTS="${EXTRA_OPTS:-}"
SQUEEZE2DIRETTA="$INSTALL_DIR/squeeze2diretta"
SQUEEZELITE="$INSTALL_DIR/squeezelite"

# Build command
CMD="$SQUEEZE2DIRETTA"
CMD="$CMD --squeezelite $SQUEEZELITE"
CMD="$CMD -s $LMS_SERVER"
CMD="$CMD --target $TARGET"
CMD="$CMD -n $PLAYER_NAME"
CMD="$CMD -r $MAX_SAMPLE_RATE"

# DSD format
# dop = DoP mode (for Roon)
# u32be/u32le = native DSD (for LMS)
if [ "$DSD_FORMAT" = "dop" ]; then
    CMD="$CMD -D"
elif [ "$DSD_FORMAT" = "u32be" ] || [ "$DSD_FORMAT" = "u32le" ]; then
    CMD="$CMD -D :$DSD_FORMAT"
fi

# PCM sample format (bit depth)
if [ "$SAMPLE_FORMAT" != "32" ] && [ -n "$SAMPLE_FORMAT" ]; then
    CMD="$CMD -a $SAMPLE_FORMAT"
fi

# WAV/AIFF header parsing
if [ "$WAV_HEADER" = "yes" ] || [ "$WAV_HEADER" = "true" ] || [ "$WAV_HEADER" = "1" ]; then
    CMD="$CMD -W"
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
echo "  Starting squeeze2diretta v1.0.1"
echo "════════════════════════════════════════════════════════"
echo ""
echo "Configuration:"
echo "  LMS Server:       $LMS_SERVER"
echo "  Diretta Target:   $TARGET"
echo "  Player Name:      $PLAYER_NAME"
echo "  Max Sample Rate:  $MAX_SAMPLE_RATE"
echo "  DSD Format:       $DSD_FORMAT"
echo "  Sample Format:    ${SAMPLE_FORMAT}-bit"
echo "  WAV Header:       $WAV_HEADER"
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
    sleep 0.5

    # URL-encode the player name (replace spaces with %20)
    ENCODED_NAME=$(echo "$PLAYER_NAME" | sed 's/ /%20/g')

    # Send pause command via LMS CLI (port 9090)
    # Format: <playerid> pause 1
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
