#!/bin/bash

BACKEND="io_uring"
MODE="debug"
DEVICE="/dev/nvme1n1"
DUCKDB="../../build/${MODE}/duckdb"

run_duckdb() {
    local sql_query="$1"
    
    # Prepend the necessary setup SQL
    local setup_sql="
        ATTACH DATABASE 'nvmefs://example.db' AS nvme (READ_WRITE); USE nvme;
    "

    $DUCKDB -c "${setup_sql} ${sql_query}"
}