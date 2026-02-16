#!/bin/bash

source ./utils.sh

BACKEND="posix"
MODE="release"
DEVICE="/dev/nvme0"
SPDK_DEVICE="0000:02:00.0" # Samsung
NSID=1

DEFAULT_DEVICE="${DEVICE}n${NSID}"

DUCKDB="../../build/${MODE}/duckdb"

if [[ "$BACKEND" == "spdk" || "$BACKEND" == "spdk_async" ]]; then
    TARGET_BACKEND=$SPDK_DEVICE
else
    TARGET_BACKEND=$DEFAULT_DEVICE
fi

echo "Setting up database with backend: $BACKEND"
echo "Using target backend: $TARGET_BACKEND"


setup_environment $BACKEND

$DUCKDB -c "CREATE OR REPLACE PERSISTENT SECRET nvmefs (TYPE NVMEFS, nvme_device_path '$TARGET_BACKEND', backend '$BACKEND');"

$DUCKDB -c "ATTACH DATABASE 'nvmefs://example.db' AS nvme (READ_WRITE); USE nvme;"

$DUCKDB -c "
ATTACH DATABASE 'nvmefs://example.db' AS nvme (READ_WRITE); USE nvme;

CREATE TABLE IF NOT EXISTS test_table (id INTEGER, data VARCHAR);
INSERT INTO test_table VALUES (1, 'Hello'), (2, 'NVMeFS'), (3, 'World');
"

$DUCKDB -c "ATTACH DATABASE 'nvmefs://example.db' AS nvme (READ_WRITE); USE nvme;"

$DUCKDB -c "
ATTACH DATABASE 'nvmefs://example.db' AS nvme (READ_WRITE); USE nvme;

SELECT * FROM test_table;
"

echo "Database setup complete."