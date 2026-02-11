#!/bin/bash
source ./common.sh

# Define output file
OUTPUT_FILE="sf30-new-perf.csv"

# Initialize CSV with header
echo "Query,Iteration,DTLB_Misses,LLC_Misses,Duration" > "$OUTPUT_FILE"

# Note: You may need to run this script with sudo or adjust /proc/sys/kernel/perf_event_paranoid
# to allow perf to collect stats.

for query in $(seq 1 22); do
    echo "Benchmarking TPCH Query $query..."
    for j in $(seq 1 3); do
        
        # Temporary file to capture perf output
        PERF_TMP=$(mktemp)
        
        start_time=$(date +%s.%N)

        # Run perf stat
        # -e: Select events (dTLB misses and Last Level Cache misses)
        # -x,: Output in CSV format
        # -o: Write stats to temp file
        perf stat -e dTLB-load-misses,LLC-load-misses -x, -o "$PERF_TMP" \
        $DUCKDB -c "
            SET threads = 16;
            SET memory_limit = '500MB';
            ATTACH 'nvmefs://sf30.db' AS nvme;
            USE nvme;
            LOAD tpch;
            PRAGMA tpch($query);
        " > /dev/null 2>&1

        end_time=$(date +%s.%N)
        duration=$(echo "$end_time - $start_time" | bc)

        # Extract values from perf output
        # perf -x, format is usually: value,unit,event,runtime,pct
        dtlb_misses=$(grep "dTLB-load-misses" "$PERF_TMP" | cut -d',' -f1)
        llc_misses=$(grep "LLC-load-misses" "$PERF_TMP" | cut -d',' -f1)

        # specific handling if values are empty (e.g., event not supported)
        dtlb_misses=${dtlb_misses:-0}
        llc_misses=${llc_misses:-0}
        
        echo "$query,$j,$dtlb_misses,$llc_misses,$duration" >> "$OUTPUT_FILE"
        echo "  Iteration $j: dTLB: $dtlb_misses | LLC: $llc_misses | Time: ${duration}s"
        
        rm -f "$PERF_TMP"
    done
done