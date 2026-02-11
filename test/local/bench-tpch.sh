source ./common.sh

echo "Query,Iteration,Duration" > results.csv

for query in $(seq 1 22); do
    echo "Benchmarking TPCH Query $query..."
    for j in $(seq 1 3); do
        
        # 1. Capture start time
        # %s.%N gives seconds.nanoseconds
        start_time=$(date +%s.%N)

        # 2. Run the workload
        # Using > /dev/null to hide output so it doesn't clutter the terminal
        duration=$($DUCKDB -csv -noheader -c "
            ATTACH DATABASE 'nvmefs://example.db' AS nvme (READ_WRITE); USE nvme;
            SET enable_profiling = 'json';
            PRAGMA tpch($query);
            " | grep "latency" | awk -F',' '{print $2}')
                    echo "  Iter $j: ${duration}s"
        
        # Log to file
        echo "$query,$j,$duration" >> results.csv
    done
done