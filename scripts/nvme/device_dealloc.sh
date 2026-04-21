#!/bin/bash

DEV=${1:-/dev/nvme0}
NSID=${2:-1}

echo "Using:"
echo "  Device: $DEV"
echo "  Namespace ID: $NSID"

if [ ! -e "$DEV" ]; then
    echo "Error: Device $DEV does not exist"
    exit 1
fi

# Get the namespace size in blocks
NSZE=$(nvme id-ns $DEV -n $NSID 2>/dev/null | grep "nsze" | awk '{print $3}')

if [ -z "$NSZE" ]; then
    echo "Error: Could not get namespace size for namespace $NSID on $DEV"
    exit 1
fi

nvme list
echo "About to deallocate $NSZE blocks on ${DEV}n${NSID}"
read -r -p "Are you sure you want to continue? [y/N] " CONFIRM

case "$CONFIRM" in
    [yY])
        ;;
    *)
        echo "Aborted"
        exit 0
        ;;
esac

# Deallocate all blocks (trim/discard)
nvme dsm ${DEV}n${NSID} --namespace-id=$NSID --ad -s 0 -b $NSZE
echo "Deallocation complete"
