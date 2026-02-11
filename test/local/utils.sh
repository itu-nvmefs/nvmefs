SECRET_FILE="$HOME/.duckdb/stored_secrets/nvmefs.duckdb_secret"
SCRIPT_DIR="../../scripts/nvme"

if [[ $EUID -ne 0 ]]; then
   echo "This benchmark script must be run with elevated permissions (sudo)."
   exit 1
fi


secret_cleanup() {
    rm -f "$SECRET_FILE"
}

disk_cleanup() {
    if [[ -f "$SCRIPT_DIR/device_dealloc.sh" && -f "$SCRIPT_DIR/create_device.sh" ]]; then
        bash "$SCRIPT_DIR/device_dealloc.sh"
        bash "$SCRIPT_DIR/create_device.sh"
    else
        echo "Warning: Cleanup scripts not found in $SCRIPT_DIR"
    fi
}

spdk_init() {
    local hugemem=4096
    HUGEMEM=$hugemem xnvme-driver
}

spdk_driver_reset() {
    xnvme-driver reset
}

setup_environment() {
    local current_target=$1
    local is_spdk=false

    echo "=== Setting up environment for target: $current_target ==="

    # Determine if SPDK is used
    if [[ "$current_target" == "spdk_sync" || "$current_target" == "spdk_async" ]]; then
        is_spdk=true
    fi

    # Check if NVMe device is present
    if command -v jq &> /dev/null; then
        local found
        found=$(nvme list -o json | jq -r --arg DEV "$DEFAULT_DEVICE" \
            '.Devices[] | select(.DevicePath == $DEV or .GenericPath == $DEV) | .DevicePath')
        
        if [[ -z "$found" ]]; then
            echo "NVMe device $DEFAULT_DEVICE not found. Resetting driver..."
            spdk_driver_reset
        fi
    else
        echo "Error: 'jq' is not installed. Cannot parse nvme output safely."
        exit 1
    fi

    # Cleanup
    echo "Performing disk cleanup..."
    disk_cleanup
    secret_cleanup

    # SPDK Init if required
    if [ "$is_spdk" = true ]; then
        echo "Initializing SPDK environment..."
        spdk_init $MEMORY_MB
    fi
    
    echo "=== Setup complete for $current_target ==="
}
