#!/bin/bash

source ./utils.sh

BACKEND="spdk_sync"
MODE="debug"
DEVICE="0000:5e:00.0"
DUCKDB="../../build/${MODE}/duckdb"
DB="nvmefs://sf30.db"

setup_environment "spdk_sync"

$DUCKDB -c "CREATE OR REPLACE PERSISTENT SECRET nvmefs (TYPE NVMEFS, nvme_device_path '$DEVICE', backend '$BACKEND');"

$DUCKDB -c "ATTACH DATABASE '$DB' AS nvme (READ_WRITE); USE nvme;"

$DUCKDB -c "
ATTACH DATABASE '$DB' AS nvme (READ_WRITE); USE nvme;

CREATE TABLE IF NOT EXISTS test_table (id INTEGER, data VARCHAR);
INSERT INTO test_table VALUES (1, 'Hello'), (2, 'NVMeFS'), (3, 'World');
"

$DUCKDB -c "
ATTACH DATABASE '$DB' AS nvme (READ_WRITE); USE nvme;

SELECT * FROM test_table;
"

echo "Database setup complete."