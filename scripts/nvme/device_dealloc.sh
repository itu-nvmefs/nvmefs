#!/bin/bash

nvme list

read -p "Enter the NVMe device number (e.g. 0): " DEV_NUM
read -p "Enter the namespace ID (e.g. 1): " NSID

DEV="/dev/nvme${DEV_NUM}"

echo "Targeting:"
echo "  Device: $DEV"
echo "  Namespace ID: $NSID"
echo "  Path: ${DEV}n${NSID}"

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

echo "WARNING: About to deallocate $NSZE blocks on ${DEV}n${NSID}"
read -r -p "Are you sure you want to continue? [y/N] " CONFIRM

case "$CONFIRM" in
    [yY])
        ;;
    *)
        echo "Aborted"
        exit 0
        ;;
esac

# Deallocate all blocks (Data Set Management)
nvme dsm "${DEV}n${NSID}" --namespace-id="$NSID" --ad -s 0 -b "$NSZE"

echo "Deallocation complete"
