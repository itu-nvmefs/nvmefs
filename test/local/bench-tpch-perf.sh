#!/bin/bash

source ./utils.sh


OUTPUT_FILE="tpch-scaling-comparison1.csv"

# Added SF column to header
echo "SF,Query,Iteration,DTLB_Misses,LLC_Misses,Duration" > "$OUTPUT_FILE"

SCALING_FACTORS=(1 10 30 100)

# backend = spdk_sync and spdk_async
for sf in "${SCALING_FACTORS[@]}"; do
    for BACKEND in "spdk_sync" "spdk_async"; do
        echo "======================================================="
        echo " PROCESSING SCALING FACTOR: SF$sf "
        echo "======================================================="

        MODE="release"
        DEVICE="/dev/nvme0"
        SPDK_DEVICE="0000:02:00.0" # Samsung
        NSID=1

        DEFAULT_DEVICE="${DEVICE}n${NSID}"

        DUCKDB="../../build/${MODE}/duckdb"

        if [[ "$BACKEND" == "spdk_sync" || "$BACKEND" == "spdk_async" ]]; then
            TARGET_BACKEND=$SPDK_DEVICE
        else
            TARGET_BACKEND=$DEFAULT_DEVICE
        fi


        setup_environment $BACKEND

        # Define paths
        # We assume 'sf${sf}_source.db' is your existing TPCH data
        SOURCE_DB="/srv/tpch-sf$sf.db" 
        TARGET_DB="nvmefs://example.db"

        MODE="release"
        BINARY="../../build/${MODE}/duckdb"

        # --- ONE-TIME SETUP PER SF ---
        # This mimics your copy_to_db logic using the DuckDB CLI
        echo "Ensuring SF$sf data is initialized..."

        $BINARY -c "
            CREATE OR REPLACE PERSISTENT SECRET nvmefs (TYPE NVMEFS, nvme_device_path '$TARGET_BACKEND', backend '$BACKEND');
        
        "

        $BINARY -c "    
            ATTACH DATABASE '$TARGET_DB' AS nvme (READ_WRITE); USE nvme;
            ATTACH '$SOURCE_DB' AS source_db (READ_ONLY);
            COPY FROM DATABASE source_db TO nvme;
            INSTALL tpch; LOAD tpch;
        "

        for query in $(seq 1 22); do
            for j in $(seq 1 3); do
                PERF_TMP=$(mktemp)
                DUCK_STATS=$(mktemp)

                # Use the SF-specific database
                perf stat -e dTLB-load-misses,LLC-load-misses -x, -o "$PERF_TMP" \
                "$BINARY" -c "
                    ATTACH DATABASE '$TARGET_DB' AS nvme;
                    USE nvme;
                    LOAD tpch;
                    SET memory_limit = '2000MB';
                    
                    PRAGMA enable_profiling = 'json';
                    PRAGMA profiling_mode = 'detailed';
                    PRAGMA profile_output = '$DUCK_STATS';
                    
                    PRAGMA tpch($query);
                " 

                # Parse pure latency
                # 1. Capture Latency properly
                # We use -r to get raw text and check if the key exists
                # print contents of ile
                duration=$(jq -r '.latency // 0' "$DUCK_STATS")

                # 2. Capture Hardware Counters
                # perf stat -x, outputs: value,unit,event,time_enabled,time_running...
                # We want the first field only, and we need to strip any non-numeric characters (like commas or spaces)
                dtlb_misses=$(grep "dTLB-load-misses" "$PERF_TMP" | cut -d',' -f1 | tr -d '[:space:]')
                llc_misses=$(grep "LLC-load-misses" "$PERF_TMP" | cut -d',' -f1 | tr -d '[:space:]')

                # If these are still empty (e.g. perf failed), default to 0
                dtlb_misses=${dtlb_misses:-0}
                llc_misses=${llc_misses:-0}

                # 3. Clean the CSV echo
                # Use a single echo string to ensure it stays on one line
                echo "${BACKEND},${sf},${query},${j},${dtlb_misses},${llc_misses},${duration}" >> "$OUTPUT_FILE"

                # 4. Clean the Printf
                # Ensure we are passing exactly the variables expected
                printf " [SF%s][%s] Q%s-Iter%d: %ss | dTLB: %s\n" "$sf" "$BACKEND" "$query" "$j" "$duration" "$dtlb_misses"
                rm -f "$PERF_TMP" "$DUCK_STATS"
            done
        done
    done
done

echo "Done! Results saved to $OUTPUT_FILE"