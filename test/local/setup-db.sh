#!/bin/bash

source ./utils.sh

BACKEND="io_uring"
MODE="debug"
DEVICE="/dev/nvme1n1"
DUCKDB="../../build/${MODE}/duckdb"

setup_environment "io_uring"

$DUCKDB -c "CREATE OR REPLACE PERSISTENT SECRET nvmefs (TYPE NVMEFS, nvme_device_path '$DEVICE', backend '$BACKEND');"

$DUCKDB -c "ATTACH DATABASE 'nvmefs://example.db' AS nvme (READ_WRITE); USE nvme;"

$DUCKDB -c "
ATTACH DATABASE 'nvmefs://example.db' AS nvme (READ_WRITE); USE nvme;

CREATE TABLE IF NOT EXISTS test_table (id INTEGER, data VARCHAR);
INSERT INTO test_table VALUES (1, 'Hello'), (2, 'NVMeFS'), (3, 'World');
"

$DUCKDB -c "
ATTACH DATABASE 'nvmefs://example.db' AS nvme (READ_WRITE); USE nvme;

SELECT * FROM test_table;
"

echo "Database setup complete."