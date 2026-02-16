#!/bin/bash

DEV=${1:-/dev/nvme1}
NSID=${2:-1}

echo "Using:"
echo "  Device: $DEV"
echo "  Namespace ID: $NSID"

# Check device exists
if [ ! -e "$DEV" ]; then
    echo "Error: Device $DEV does not exist"
    exit 1
fi

# Get the namespace size in blocks
NSZE=$(sudo nvme id-ns $DEV -n $NSID 2>/dev/null | grep "nsze" | awk '{print $3}')

if [ -z "$NSZE" ]; then
    echo "Error: Could not get namespace size for namespace $NSID on $DEV"
    exit 1
fi

echo "Deallocating $NSZE blocks on ${DEV}n${NSID}"

# Deallocate all blocks (trim/discard)
sudo nvme dsm ${DEV}n${NSID} --namespace-id=$NSID --ad -s 0 -b $NSZE

echo "Deallocation complete"