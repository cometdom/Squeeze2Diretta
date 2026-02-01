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
# Note: DSD format (-D :u32be) is hardcoded in squeeze2diretta for Diretta compatibility

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
echo "  DSD Format:       :u32be (hardcoded)"
echo ""
echo "Command:"
echo "  $CMD"
echo ""
echo "════════════════════════════════════════════════════════"
echo ""

# Execute
exec $CMD
