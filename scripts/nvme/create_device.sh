#!/bin/bash

# Parse command line arguments
DEV=${1:-/dev/nvme1}
NSID=${2:-1}

echo "Using defaults:"
echo "  Device: $DEV"
echo "  Namespace ID: $NSID"

# Check if device exists
if [ ! -e "$DEV" ]; then
    echo "Error: Device $DEV does not exist"
    exit 1
fi

# --- Check namespace management capability (OACS bit 3)
OACS=$(nvme id-ctrl $DEV | grep "oacs" | awk '{print $3}')
# Remove '0x' prefix if present before adding it back
OACS=${OACS#0x}
OACS_NS_MGMT=$((0x$OACS & 0x8))

# Get block size
LBADS=$(nvme id-ns $DEV -n $NSID 2>/dev/null | grep "lbaf  0" | awk -F 'lbads:' '{print $2}' | awk '{print $1}')
if [ -z "$LBADS" ]; then
    echo "Error: Could not get block size from namespace $NSID"
    exit 1
fi
BLOCK_SIZE=$((2 ** LBADS))

# Calculate number of blocks from tnvmcap
TNVM=$(nvme id-ctrl $DEV | grep tnvmcap | sed 's/,//g' | awk '{print $3}')
if [[ -z "$TNVM" ]]; then
    echo "Error: Could not read tnvmcap."
    exit 1
fi

NUM_BLOCKS=$(python3 - <<EOF
tnvm = $TNVM
bs = $BLOCK_SIZE
print(int(tnvm // bs))
EOF
)

echo "Block size     : $BLOCK_SIZE bytes"
echo "Device size    : $TNVM bytes"
echo "NUM_BLOCKS     : $NUM_BLOCKS"

if [[ "$OACS_NS_MGMT" -eq 0 ]]; then
    echo "Device does not support namespace management. Using existing namespace $NSID."
    echo "Done."
    exit 0
fi

echo "Device supports namespace management. Creating namespace..."

# Set feature (Namespace Management)
echo "Setting feature 0x1D..."
nvme set-feature $DEV -f 0x1D -c 0 -s

# Create namespace
echo "Creating namespace with $NUM_BLOCKS blocks..."
nvme create-ns $DEV -b $BLOCK_SIZE --nsze=$NUM_BLOCKS --ncap=$NUM_BLOCKS

# Attach namespace
echo "Attaching namespace $NSID to controller..."
nvme attach-ns $DEV --namespace-id=$NSID --controllers=0x7

echo "Done. Namespace $NSID created and attached."