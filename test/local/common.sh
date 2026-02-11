#!/bin/bash

BACKEND="io_uring"
MODE="release"
#DEVICE="/dev/nvme1n1"
DUCKDB="../../../nvmefs/build/${MODE}/duckdb"

run_duckdb() {
    local sql_query="$1"
    
    # Prepend the necessary setup SQL
    local setup_sql="
        ATTACH DATABASE 'nvmefs://sf30.db' AS nvme (READ_WRITE); USE nvme;
    "

    $DUCKDB -c "${setup_sql} ${sql_query}"
}