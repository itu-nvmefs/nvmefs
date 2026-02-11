#!/bin/bash
source ./common.sh

# Define output file
OUTPUT_FILE="sf30-comparison-perf-final2.csv"

# Initialize CSV with header
echo "Version,Query,Iteration,DTLB_Misses,LLC_Misses,Duration" > "$OUTPUT_FILE"

# List of queries to benchmark
QUERIES=("9" "10" "13" "18" "21" "1")
# Map versions to their respective binary paths
VERSIONS=("new", "old")

for query in "${QUERIES[@]}"; do
    echo "-------------------------------------------------------"
    echo "Benchmarking TPCH Query $query..."
    echo "-------------------------------------------------------"

    for j in $(seq 1 3); do
    
        for version in "${VERSIONS[@]}"; do
            # Set the binary path based on the version
            if [ "$version" == "old" ]; then
                BINARY="../../../nvmefs/build/${MODE}/duckdb"
            else
                BINARY="../../../nvmefs-deniz/build/${MODE}/duckdb"
            fi
            # Temporary file to capture perf output
            PERF_TMP=$(mktemp)
            
            start_time=$(date +%s.%N)

            # Execute with perf stat
            perf stat -e dTLB-load-misses,LLC-load-misses -x, -o "$PERF_TMP" \
            "$BINARY" -c "
                ATTACH 'nvmefs://sf30.db' AS nvme;
                USE nvme;
                LOAD tpch;
                SET threads = 16;
                SET memory_limit = '2000MB';
                PRAGMA tpch($query);
            " > /dev/null 2>&1

            end_time=$(date +%s.%N)
            duration=$(echo "$end_time - $start_time" | bc)

            # Extract values from perf output (Column 1 is the value)
            dtlb_misses=$(grep "dTLB-load-misses" "$PERF_TMP" | cut -d',' -f1)
            llc_misses=$(grep "LLC-load-misses" "$PERF_TMP" | cut -d',' -f1)

            # Default to 0 if the event was not captured
            dtlb_misses=${dtlb_misses:-0}
            llc_misses=${llc_misses:-0}
            
            # Save to CSV
            echo "$version,$query,$j,$dtlb_misses,$llc_misses,$duration" >> "$OUTPUT_FILE"
            
            # Log progress to terminal
            printf " [%s] Iter %d: Time: %.3fs | dTLB: %s | LLC: %s\n" \
                "$version" "$j" "$duration" "$dtlb_misses" "$llc_misses"
            
            rm -f "$PERF_TMP"
        done
    done
done

echo "-------------------------------------------------------"
echo "Done! Results saved to $OUTPUT_FILE"