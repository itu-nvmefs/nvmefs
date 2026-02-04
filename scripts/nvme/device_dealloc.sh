#!/bin/bash

DEV=/dev/nvme1
NSID=1

# Get the namespace size in blocks
NSZE=$(sudo nvme id-ns $DEV -n $NSID 2>/dev/null | grep "nsze" | awk '{print $3}')

if [ -z "$NSZE" ]; then
    echo "Error: Could not get namespace size"
    exit 1
fi

echo "Deallocating $NSZE blocks on ${DEV}n${NSID}"

# Deallocate all blocks (trim/discard)
sudo nvme dsm ${DEV}n${NSID} --namespace-id=$NSID --ad -s 0 -b $NSZE

echo "Deallocation complete"