#!/bin/bash

source ./common.sh

SF=30

echo "Setting up TPCH data with scale factor $SF, this may take a while..."

run_duckdb "
LOAD tpch;
SET threads = 124;
CALL dbgen(sf = $SF);
"

echo "TPCH data setup complete."