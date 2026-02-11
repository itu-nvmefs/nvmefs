#!/bin/bash

source ./common.sh

SF=$1

if [ -z "$1" ]; then
    echo "Usage: $0 <tpch-data-dir>"
    exit 1
fi

echo "Setting up TPCH data with scale factor $SF, this may take a while..."

run_duckdb "
LOAD tpch;
SET threads = 64;
CALL dbgen(sf = $SF);
"

echo "TPCH data setup complete."