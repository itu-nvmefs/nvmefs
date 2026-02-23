#!/bin/bash
source ./common.sh

OUTPUT="sf30-final.csv"

echo "Version,Query,Iteration,Duration" > $OUTPUT

for query in "9" "10" "13" "18" "21" "1"; do
    echo "Benchmarking TPCH Query $query..."

    DUCKDB="../../../nvmefs/build/${MODE}/duckdb"

    for j in $(seq 1 3); do
        start_time=$(date +%s.%N)

        $DUCKDB -c "
            ATTACH 'nvmefs://sf30.db' AS nvme;
            USE nvme;
            LOAD tpch;
            SET threads = 16;
            SET memory_limit = '2000MB';
            PRAGMA tpch($query);
        " > /dev/null 2>&1

        end_time=$(date +%s.%N)

        duration=$(echo "$end_time - $start_time" | bc)
        
        echo "old,$query,$j,$duration" >> $OUTPUT
        echo "  Old Iteration $j: ${duration}s"
    done

    DUCKDB="../../../nvmefs-deniz/build/${MODE}/duckdb"

    for j in $(seq 1 3); do
        start_time=$(date +%s.%N)

        $DUCKDB -c "
            ATTACH 'nvmefs://sf30.db' AS nvme;
            USE nvme;
            LOAD tpch;
            SET threads = 16;
            SET memory_limit = '2000MB';
            PRAGMA tpch($query);
        " > /dev/null 2>&1

        end_time=$(date +%s.%N)

        duration=$(echo "$end_time - $start_time" | bc)
        
        echo "new,$query,$j,$duration" >> $OUTPUT
        echo " New Iteration $j: ${duration}s"
    done
done