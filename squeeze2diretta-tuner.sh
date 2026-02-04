#!/bin/bash
#
# squeeze2diretta-tuner.sh
# Complete CPU isolation and tuning for squeeze2diretta.service
#
# This script implements three layers of optimization:
#   1. SMT disabled (nosmt) - physical cores only
#   2. CPU isolation (isolcpus) - audio cores isolated, core 0 for housekeeping
#   3. Thread distribution - spread threads across isolated cores
#
# squeeze2diretta spawns a child squeezelite process, so we tune BOTH.
#
# Usage:
#   sudo ./squeeze2diretta-tuner.sh apply
#   sudo reboot
#   sudo ./squeeze2diretta-tuner.sh verify
#   sudo ./squeeze2diretta-tuner.sh distribute
#   sudo ./squeeze2diretta-tuner.sh status

set -euo pipefail

# =============================================================================
# CONFIGURATION - Adjust for your CPU
# =============================================================================

# Default: 4-core system (Raspberry Pi 4/5)
# For 8-core systems, use: HOUSEKEEPING_CPU="0" AUDIO_CPUS="1-7" AUDIO_CPUS_LIST="1 2 3 4 5 6 7"
HOUSEKEEPING_CPU="${HOUSEKEEPING_CPU:-0}"
AUDIO_CPUS="${AUDIO_CPUS:-1-3}"
AUDIO_CPUS_LIST="${AUDIO_CPUS_LIST:-1 2 3}"

# Auto-detect if we have more cores
CORE_COUNT=$(nproc)
if [[ "$CORE_COUNT" -ge 8 ]] && [[ "$AUDIO_CPUS" == "1-3" ]]; then
    AUDIO_CPUS="1-7"
    AUDIO_CPUS_LIST="1 2 3 4 5 6 7"
fi

# =============================================================================
# PATHS AND CONSTANTS
# =============================================================================

GRUB_FILE="/etc/default/grub"
SYSTEMD_DIR="/etc/systemd/system"
LOCAL_BIN="/usr/local/bin"
INSTALL_DIR="/opt/squeeze2diretta"

detect_grub_cfg() {
    if [[ -f /boot/grub2/grub.cfg ]]; then
        echo "/boot/grub2/grub.cfg"
    elif [[ -f /boot/grub/grub.cfg ]]; then
        echo "/boot/grub/grub.cfg"
    else
        echo "/boot/grub2/grub.cfg"
    fi
}
GRUB_CFG=$(detect_grub_cfg)

SERVICE_NAME="squeeze2diretta.service"
SLICE_NAME="audio-isolated.slice"
DIST_SCRIPT="${LOCAL_BIN}/distribute-squeeze2diretta-threads.sh"

# =============================================================================
# UTILITY FUNCTIONS
# =============================================================================

check_root() {
    if [[ "${EUID}" -ne 0 ]]; then
        echo "ERROR: This script must be run as root (sudo)" >&2
        exit 1
    fi
}

show_grub() {
    echo "GRUB_CMDLINE_LINUX:"
    grep "^GRUB_CMDLINE_LINUX=" "$GRUB_FILE" 2>/dev/null | sed 's/^/  /' || echo "  (not found)"
}

get_service_pid() {
    systemctl show "${SERVICE_NAME}" -p MainPID --value 2>/dev/null || echo ""
}

# Get both squeeze2diretta and its child squeezelite PIDs
get_all_pids() {
    local main_pid
    main_pid=$(get_service_pid)
    if [[ -z "$main_pid" || "$main_pid" == "0" ]]; then
        return
    fi
    echo "$main_pid"
    # Find child processes (squeezelite)
    pgrep -P "$main_pid" 2>/dev/null || true
}

# =============================================================================
# APPLY - Configures all three layers
# =============================================================================

do_apply() {
    echo "═══════════════════════════════════════════════════════════════"
    echo "  squeeze2diretta CPU Tuner - Apply Configuration"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""
    echo "Configuration:"
    echo "  SMT:          DISABLED (nosmt)"
    echo "  Housekeeping: CPU ${HOUSEKEEPING_CPU}"
    echo "  Audio:        CPUs ${AUDIO_CPUS} (isolated)"
    echo "  Core count:   ${CORE_COUNT}"
    if [[ -d /sys/firmware/efi ]]; then
        echo "  Boot mode:    UEFI"
    else
        echo "  Boot mode:    BIOS/Legacy"
    fi
    echo "  GRUB config:  ${GRUB_CFG}"
    echo ""

    # Backup GRUB
    if [[ -f "$GRUB_FILE" ]]; then
        cp "$GRUB_FILE" "${GRUB_FILE}.backup.$(date +%Y%m%d-%H%M%S)"
        echo "GRUB backup created"
    fi
    echo ""

    echo "BEFORE:"
    show_grub
    echo ""

    # -------------------------------------------------------------------------
    # Layer 1 & 2: Kernel parameters (nosmt + isolation)
    # -------------------------------------------------------------------------
    echo "Configuring kernel parameters..."

    if [[ -f "$GRUB_FILE" ]]; then
        # Remove existing parameters
        sed -i -E 's/ ?nosmt//g' "$GRUB_FILE"
        sed -i -E 's/ ?isolcpus=[^ "]+//g' "$GRUB_FILE"
        sed -i -E 's/ ?nohz_full=[^ "]+//g' "$GRUB_FILE"
        sed -i -E 's/ ?nohz=[^ "]+//g' "$GRUB_FILE"
        sed -i -E 's/ ?rcu_nocbs=[^ "]+//g' "$GRUB_FILE"
        sed -i -E 's/ ?irqaffinity=[^ "]+//g' "$GRUB_FILE"

        # Build new parameters
        local kernel_params="nosmt isolcpus=${AUDIO_CPUS} rcu_nocbs=${AUDIO_CPUS} irqaffinity=${HOUSEKEEPING_CPU}"

        # Add to GRUB_CMDLINE_LINUX
        if grep -qE '^GRUB_CMDLINE_LINUX=""' "$GRUB_FILE"; then
            sed -i "s/^GRUB_CMDLINE_LINUX=\"\"/GRUB_CMDLINE_LINUX=\"${kernel_params}\"/" "$GRUB_FILE"
        else
            sed -i "s/^GRUB_CMDLINE_LINUX=\"/GRUB_CMDLINE_LINUX=\"${kernel_params} /" "$GRUB_FILE"
        fi

        # Clean up multiple spaces
        sed -i 's/  */ /g' "$GRUB_FILE"

        echo "AFTER:"
        show_grub
        echo ""

        # Update GRUB
        echo "Updating GRUB bootloader..."
        if command -v update-grub &> /dev/null; then
            update-grub
        elif command -v grub2-mkconfig &> /dev/null; then
            grub2-mkconfig -o "${GRUB_CFG}"
        else
            echo "WARNING: Could not find grub update command"
        fi
    else
        echo "NOTE: /etc/default/grub not found"
        echo "For Raspberry Pi, add to /boot/cmdline.txt or /boot/firmware/cmdline.txt:"
        echo "  isolcpus=${AUDIO_CPUS} rcu_nocbs=${AUDIO_CPUS} irqaffinity=${HOUSEKEEPING_CPU}"
    fi
    echo ""

    # -------------------------------------------------------------------------
    # Layer 2b: Systemd slice for CPU pinning
    # -------------------------------------------------------------------------
    echo "Creating systemd slice: ${SLICE_NAME}"
    cat << EOF > "${SYSTEMD_DIR}/${SLICE_NAME}"
[Unit]
Description=Isolated CPU slice for audio processing

[Slice]
AllowedCPUs=${AUDIO_CPUS}
CPUQuota=100%
EOF

    # -------------------------------------------------------------------------
    # Layer 2c: Service override
    # -------------------------------------------------------------------------
    echo "Creating service override..."
    local override_dir="${SYSTEMD_DIR}/${SERVICE_NAME}.d"
    mkdir -p "$override_dir"

    cat << EOF > "${override_dir}/cpu-tuning.conf"
[Service]
# Run on isolated audio CPUs
Slice=${SLICE_NAME}

# Real-time scheduling
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=80

# High priority
Nice=-15

# Memory locking (required for RT audio)
LimitMEMLOCK=infinity
LimitRTPRIO=99

# Distribute threads after startup (with delay for squeezelite to spawn)
ExecStartPost=${DIST_SCRIPT}
EOF

    # -------------------------------------------------------------------------
    # Layer 3: Thread distribution script
    # -------------------------------------------------------------------------
    echo "Creating thread distribution script: ${DIST_SCRIPT}"
    cat << 'SCRIPT' > "${DIST_SCRIPT}"
#!/bin/bash
# Distribute squeeze2diretta threads across isolated CPUs
# Called automatically by systemd after service start
# Handles both squeeze2diretta (parent) and squeezelite (child) processes

SERVICE="squeeze2diretta.service"
LOG="/var/log/squeeze2diretta-thread-distribution.log"

log() { echo "$(date '+%Y-%m-%d %H:%M:%S'): $*" >> "$LOG"; }

# Read isolated CPUs from kernel (works inside cgroups, unlike nproc)
ISOLATED=$(cat /sys/devices/system/cpu/isolated 2>/dev/null)
if [[ "$ISOLATED" == "1-7" ]]; then
    CPUS="1 2 3 4 5 6 7"
elif [[ "$ISOLATED" == "1-3" ]]; then
    CPUS="1 2 3"
else
    # Fallback: expand the range (e.g., "1-7" -> "1 2 3 4 5 6 7")
    if [[ "$ISOLATED" =~ ^([0-9]+)-([0-9]+)$ ]]; then
        start=${BASH_REMATCH[1]}
        end=${BASH_REMATCH[2]}
        CPUS=$(seq -s ' ' "$start" "$end")
    else
        # Default fallback
        CPUS="1 2 3"
    fi
fi

# Wait for squeezelite child process to spawn and create threads
sleep 3

MAIN_PID=$(systemctl show "$SERVICE" -p MainPID --value 2>/dev/null)
if [[ -z "$MAIN_PID" || "$MAIN_PID" == "0" ]]; then
    log "Service not running, exiting"
    exit 0
fi

log "Main PID: $MAIN_PID"

# Find squeezelite child process
CHILD_PIDS=$(pgrep -P "$MAIN_PID" 2>/dev/null || true)

log "Distributing threads for squeeze2diretta (PID $MAIN_PID)"
if [[ -n "$CHILD_PIDS" ]]; then
    log "Also distributing for child processes: $CHILD_PIDS"
fi

CPU_ARRAY=($CPUS)
NUM_CPUS=${#CPU_ARRAY[@]}
total_threads=0

# Function to distribute threads for a process
distribute_process() {
    local pid=$1
    local name=$2
    local i=$3

    for tid in $(ps -T -o tid= -p "$pid" 2>/dev/null); do
        cpu_idx=$((i % NUM_CPUS))
        target=${CPU_ARRAY[$cpu_idx]}
        if taskset -pc "$target" "$tid" > /dev/null 2>&1; then
            log "  [$name] TID $tid -> CPU $target"
        fi
        i=$((i + 1))
        total_threads=$((total_threads + 1))
    done
    echo $i
}

# Distribute squeeze2diretta threads
i=0
i=$(distribute_process "$MAIN_PID" "squeeze2diretta" "$i")

# Distribute squeezelite threads (offset to spread across CPUs)
for child in $CHILD_PIDS; do
    i=$(distribute_process "$child" "squeezelite" "$i")
done

log "Distribution complete: $total_threads threads across $NUM_CPUS CPUs"
SCRIPT

    chmod +x "${DIST_SCRIPT}"

    # Reload systemd
    echo "Reloading systemd..."
    systemctl daemon-reload

    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo "  Configuration Applied Successfully"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""
    echo "REBOOT REQUIRED for kernel parameters to take effect."
    echo ""
    echo "After reboot:"
    echo "  1. sudo systemctl restart ${SERVICE_NAME}"
    echo "  2. sudo $0 verify"
    echo ""
}

# =============================================================================
# VERIFY - Check all three layers
# =============================================================================

do_verify() {
    echo "═══════════════════════════════════════════════════════════════"
    echo "  squeeze2diretta CPU Tuner - Verification"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""

    local errors=0

    # -------------------------------------------------------------------------
    # Layer 1: SMT (nosmt)
    # -------------------------------------------------------------------------
    echo "Layer 1: SMT Status"
    echo "-------------------"

    if grep -q "nosmt" /proc/cmdline 2>/dev/null; then
        echo "[OK] nosmt in kernel cmdline"
    else
        echo "[INFO] nosmt NOT in kernel cmdline (may not apply to ARM)"
    fi

    if [[ -f /sys/devices/system/cpu/smt/active ]]; then
        local smt_active
        smt_active=$(cat /sys/devices/system/cpu/smt/active)
        if [[ "$smt_active" == "0" ]]; then
            echo "[OK] SMT disabled (smt/active = 0)"
        else
            echo "[INFO] SMT enabled (smt/active = $smt_active)"
        fi
    else
        echo "[INFO] SMT not applicable (no smt/active - ARM?)"
    fi

    local cpu_count
    cpu_count=$(nproc)
    echo "[INFO] CPU count: $cpu_count"
    echo ""

    # -------------------------------------------------------------------------
    # Layer 2: CPU Isolation
    # -------------------------------------------------------------------------
    echo "Layer 2: CPU Isolation"
    echo "----------------------"

    local cmdline
    cmdline=$(cat /proc/cmdline 2>/dev/null || echo "")

    if echo "$cmdline" | grep -q "isolcpus="; then
        echo "[OK] isolcpus configured"
    else
        echo "[WARN] isolcpus not in kernel cmdline"
    fi

    if [[ -f /sys/devices/system/cpu/isolated ]]; then
        local isolated
        isolated=$(cat /sys/devices/system/cpu/isolated)
        echo "[INFO] Kernel isolated CPUs: ${isolated:-none}"
    fi

    if [[ -f "${SYSTEMD_DIR}/${SLICE_NAME}" ]]; then
        echo "[OK] Systemd slice exists"
    else
        echo "[FAIL] Systemd slice missing"
        errors=$((errors + 1))
    fi

    if [[ -f "${SYSTEMD_DIR}/${SERVICE_NAME}.d/cpu-tuning.conf" ]]; then
        echo "[OK] Service override exists"
    else
        echo "[FAIL] Service override missing"
        errors=$((errors + 1))
    fi
    echo ""

    # -------------------------------------------------------------------------
    # Layer 3: Thread Distribution
    # -------------------------------------------------------------------------
    echo "Layer 3: Thread Distribution"
    echo "----------------------------"

    if [[ -f "${DIST_SCRIPT}" ]]; then
        echo "[OK] Distribution script exists"
    else
        echo "[FAIL] Distribution script missing"
        errors=$((errors + 1))
    fi

    local main_pid
    main_pid=$(get_service_pid)

    if [[ -z "$main_pid" || "$main_pid" == "0" ]]; then
        echo "[INFO] Service not running - start it to verify thread distribution"
    else
        echo "[OK] Service running (PID: $main_pid)"

        # Find squeezelite child
        local child_pids
        child_pids=$(pgrep -P "$main_pid" 2>/dev/null || true)

        echo ""
        echo "squeeze2diretta threads (PID $main_pid):"
        echo "  TID      CPU"
        echo "  -------  ---"
        while read -r tid cpu; do
            printf "  %-7s  %s\n" "$tid" "$cpu"
        done < <(ps -T -o tid=,psr= -p "$main_pid" 2>/dev/null)

        if [[ -n "$child_pids" ]]; then
            for child in $child_pids; do
                local child_name
                child_name=$(ps -o comm= -p "$child" 2>/dev/null || echo "child")
                echo ""
                echo "$child_name threads (PID $child):"
                echo "  TID      CPU"
                echo "  -------  ---"
                while read -r tid cpu; do
                    printf "  %-7s  %s\n" "$tid" "$cpu"
                done < <(ps -T -o tid=,psr= -p "$child" 2>/dev/null)
            done
        fi

        echo ""
        echo "Combined threads per CPU:"
        {
            ps -T -o psr= -p "$main_pid" 2>/dev/null
            for child in $child_pids; do
                ps -T -o psr= -p "$child" 2>/dev/null
            done
        } | sort -n | uniq -c | while read -r count cpu; do
            printf "  CPU %s: %s thread(s)\n" "$cpu" "$count"
        done
    fi
    echo ""

    # -------------------------------------------------------------------------
    # Summary
    # -------------------------------------------------------------------------
    echo "═══════════════════════════════════════════════════════════════"
    if [[ $errors -eq 0 ]]; then
        echo "  All checks passed"
    else
        echo "  $errors issue(s) detected"
    fi
    echo "═══════════════════════════════════════════════════════════════"
}

# =============================================================================
# DISTRIBUTE - Manually trigger thread distribution
# =============================================================================

do_distribute() {
    echo "═══════════════════════════════════════════════════════════════"
    echo "  Distributing Threads"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""

    local main_pid
    main_pid=$(get_service_pid)

    if [[ -z "$main_pid" || "$main_pid" == "0" ]]; then
        echo "ERROR: ${SERVICE_NAME} is not running"
        exit 1
    fi

    # Auto-detect CPU configuration
    local -a cpus
    if [[ "$CORE_COUNT" -ge 8 ]]; then
        cpus=(1 2 3 4 5 6 7)
    else
        cpus=(1 2 3)
    fi
    local num_cpus=${#cpus[@]}

    echo "Main PID: $main_pid"
    echo "Target CPUs: ${cpus[*]}"

    local child_pids
    child_pids=$(pgrep -P "$main_pid" 2>/dev/null || true)

    if [[ -n "$child_pids" ]]; then
        echo "Child PIDs: $child_pids"
    fi
    echo ""

    echo "Before distribution:"
    ps -T -o tid=,psr=,comm= -p "$main_pid" 2>/dev/null | while read -r tid cpu comm; do
        printf "  TID %-7s on CPU %-2s (%s)\n" "$tid" "$cpu" "$comm"
    done
    for child in $child_pids; do
        ps -T -o tid=,psr=,comm= -p "$child" 2>/dev/null | while read -r tid cpu comm; do
            printf "  TID %-7s on CPU %-2s (%s)\n" "$tid" "$cpu" "$comm"
        done
    done
    echo ""

    echo "Distributing (round-robin):"
    local i=0

    # Distribute squeeze2diretta threads
    for tid in $(ps -T -o tid= -p "$main_pid" 2>/dev/null); do
        local cpu_idx=$((i % num_cpus))
        local target_cpu=${cpus[$cpu_idx]}
        if taskset -pc "$target_cpu" "$tid" > /dev/null 2>&1; then
            echo "  squeeze2diretta TID $tid -> CPU $target_cpu"
        fi
        i=$((i + 1))
    done

    # Distribute squeezelite threads
    for child in $child_pids; do
        local child_name
        child_name=$(ps -o comm= -p "$child" 2>/dev/null || echo "child")
        for tid in $(ps -T -o tid= -p "$child" 2>/dev/null); do
            local cpu_idx=$((i % num_cpus))
            local target_cpu=${cpus[$cpu_idx]}
            if taskset -pc "$target_cpu" "$tid" > /dev/null 2>&1; then
                echo "  $child_name TID $tid -> CPU $target_cpu"
            fi
            i=$((i + 1))
        done
    done

    echo ""
    echo "After (may take a moment for threads to migrate):"
    sleep 1
    ps -T -o tid=,psr=,comm= -p "$main_pid" 2>/dev/null | while read -r tid cpu comm; do
        printf "  TID %-7s on CPU %-2s (%s)\n" "$tid" "$cpu" "$comm"
    done
    for child in $child_pids; do
        ps -T -o tid=,psr=,comm= -p "$child" 2>/dev/null | while read -r tid cpu comm; do
            printf "  TID %-7s on CPU %-2s (%s)\n" "$tid" "$cpu" "$comm"
        done
    done
}

# =============================================================================
# STATUS - Quick status check
# =============================================================================

do_status() {
    echo "═══════════════════════════════════════════════════════════════"
    echo "  squeeze2diretta CPU Tuner - Status"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""

    # SMT
    echo -n "SMT: "
    if [[ -f /sys/devices/system/cpu/smt/active ]]; then
        local smt=$(cat /sys/devices/system/cpu/smt/active)
        if [[ "$smt" == "0" ]]; then
            echo "DISABLED"
        else
            echo "ENABLED"
        fi
    else
        echo "N/A (ARM?)"
    fi

    # CPU count
    echo "CPUs: $(nproc)"

    # Isolation
    echo -n "Isolated: "
    cat /sys/devices/system/cpu/isolated 2>/dev/null || echo "none"

    # Service
    echo -n "Service: "
    if systemctl is-active --quiet "${SERVICE_NAME}" 2>/dev/null; then
        local main_pid=$(get_service_pid)
        echo "RUNNING (PID $main_pid)"

        local child_pids
        child_pids=$(pgrep -P "$main_pid" 2>/dev/null || true)
        if [[ -n "$child_pids" ]]; then
            echo "Children: $child_pids"
        fi

        echo ""
        echo "Threads per CPU:"
        {
            ps -T -o psr= -p "$main_pid" 2>/dev/null
            for child in $child_pids; do
                ps -T -o psr= -p "$child" 2>/dev/null
            done
        } | sort -n | uniq -c | while read -r count cpu; do
            printf "  CPU %s: %s\n" "$cpu" "$count"
        done
    else
        echo "STOPPED"
    fi
}

# =============================================================================
# REVERT - Remove all configuration
# =============================================================================

do_revert() {
    echo "═══════════════════════════════════════════════════════════════"
    echo "  squeeze2diretta CPU Tuner - Revert"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""

    if [[ -f "$GRUB_FILE" ]]; then
        echo "Removing kernel parameters..."
        sed -i -E 's/ ?nosmt//g' "$GRUB_FILE"
        sed -i -E 's/ ?isolcpus=[^ "]+//g' "$GRUB_FILE"
        sed -i -E 's/ ?nohz_full=[^ "]+//g' "$GRUB_FILE"
        sed -i -E 's/ ?nohz=[^ "]+//g' "$GRUB_FILE"
        sed -i -E 's/ ?rcu_nocbs=[^ "]+//g' "$GRUB_FILE"
        sed -i -E 's/ ?irqaffinity=[^ "]+//g' "$GRUB_FILE"
        sed -i 's/  */ /g' "$GRUB_FILE"

        show_grub
        echo ""

        echo "Updating GRUB..."
        if command -v update-grub &> /dev/null; then
            update-grub
        elif command -v grub2-mkconfig &> /dev/null; then
            grub2-mkconfig -o "${GRUB_CFG}"
        fi
    fi

    echo "Removing systemd configurations..."
    rm -f "${SYSTEMD_DIR}/${SLICE_NAME}"
    rm -rf "${SYSTEMD_DIR}/${SERVICE_NAME}.d"
    rm -f "${DIST_SCRIPT}"

    systemctl daemon-reload

    echo ""
    echo "Configuration reverted. REBOOT REQUIRED."
}

# =============================================================================
# USAGE
# =============================================================================

usage() {
    cat <<EOF
squeeze2diretta CPU Tuner
=========================

Complete CPU isolation for audio: SMT disabled, cores isolated, threads distributed.
Handles both squeeze2diretta (parent) and squeezelite (child) processes.

Usage: sudo $0 [command]

Commands:
  apply       - Apply all configuration (requires reboot)
  verify      - Check all layers are working
  distribute  - Manually distribute threads now
  status      - Quick status check
  revert      - Remove all configuration (requires reboot)

Configuration (auto-detected, override with env vars):
  HOUSEKEEPING_CPU: ${HOUSEKEEPING_CPU}
  AUDIO_CPUS:       ${AUDIO_CPUS}
  Core count:       ${CORE_COUNT}

Workflow:
  1. sudo $0 apply
  2. sudo reboot
  3. sudo systemctl restart ${SERVICE_NAME}
  4. sudo $0 verify

For Raspberry Pi:
  Kernel parameters must be added to /boot/cmdline.txt manually.
  The script will still create systemd slice and thread distribution.

EOF
}

# =============================================================================
# MAIN
# =============================================================================

case "${1:-}" in
    apply)
        check_root
        do_apply
        ;;
    verify)
        do_verify
        ;;
    distribute)
        check_root
        do_distribute
        ;;
    status)
        do_status
        ;;
    revert)
        check_root
        do_revert
        ;;
    *)
        usage
        ;;
esac
