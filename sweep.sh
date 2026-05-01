#!/usr/bin/env bash
# sweep.sh
#
# Full benchmark sweep: for every thread count, run all benchmarks (thread
# scaling, c-parameter scaling, dataset/batch-size scaling) and write every
# measurement into a single unified CSV:
#
#   results/full_sweep.csv
#
# Unified schema
#   threads   — PARLAY_NUM_THREADS used for this invocation
#   algo      — dplus1 | cbyd_c2 | cbyd_cN | dplus1_10batch | cbyd_c2_10batch
#   mode      — thread | c_scaling | vertex | batch | snap
#   dataset   — e.g. rmat_2^18 or email-Enron
#   n         — number of vertices
#   m         — number of undirected edges
#   Delta     — max degree
#   c         — palette multiplier (N/A for dplus1)
#   batch_sz  — edges per dynamic batch
#   run       — 0-indexed repetition number
#   time_s    — wall-clock seconds
#
# Usage
#   ./sweep.sh [options]
#
# Options
#   --snap-dir DIR        SNAP data directory          (default: ./data)
#   --runs N              Repetitions per data point   (default: 5)
#   --threads LIST        Comma-separated thread list  (default: 1,2,4,8,16,32,64)
#   --n N                 RMAT size for fixed benchmarks (default: 262144 = 2^18)
#   --heavy               Include com-youtube
#   --skip-build          Skip make bench step
#   --out FILE            Output CSV path             (default: results/full_sweep.csv)
#   --help

set -euo pipefail

# ── Defaults ─────────────────────────────────────────────────────────────────
SNAP_DIR="data"
RUNS=5
THREADS="1,2,4,8,16,32,64"
N_FIXED=262144   # 2^18
HEAVY_FLAG=""
SKIP_BUILD=0
OUT_DIR="results"
OUT_FILE="${OUT_DIR}/full_sweep.csv"

# ── Arg parse ─────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --snap-dir)   SNAP_DIR="$2";     shift 2 ;;
        --runs)       RUNS="$2";         shift 2 ;;
        --threads)    THREADS="$2";      shift 2 ;;
        --n)          N_FIXED="$2";      shift 2 ;;
        --heavy)      HEAVY_FLAG="--heavy"; shift ;;
        --skip-build) SKIP_BUILD=1;      shift ;;
        --out)        OUT_FILE="$2";     shift 2 ;;
        --help)
            sed -n '/^# sweep/,/^[^#]/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
mkdir -p "$OUT_DIR"

log() { printf '[sweep] %s\n' "$*" >&2; }

# ── Build ─────────────────────────────────────────────────────────────────────
if [[ $SKIP_BUILD -eq 0 ]]; then
    log "Building benchmark binaries (make bench)..."
    make bench
    log "Build done."
fi

# ── CSV header ────────────────────────────────────────────────────────────────
echo "threads,algo,mode,dataset,n,m,Delta,c,batch_sz,run,time_s" > "$OUT_FILE"
log "Output: $OUT_FILE"

# ── Helpers ───────────────────────────────────────────────────────────────────

# append_thread_scaling T
#   Runs bench_thread_scaling under PARLAY_NUM_THREADS=T and appends rows.
#   Binary CSV: algo,num_workers,n,m,run,time_s
#   We normalise to unified schema (mode=thread, dataset=rmat_fixed, c=NA).
append_thread_scaling() {
    local T="$1"
    PARLAY_NUM_THREADS="$T" ./bench_thread_scaling \
        --n "$N_FIXED" --runs "$RUNS" \
    | awk -F',' -v T="$T" '
        /^#/ { next }
        {
            algo=$1; n=$3; m=$4; run=$5; time=$6
            # derive dataset name from n
            dataset = "rmat_" n
            # derive c: algo name carries it (cbyd_c2 → 2, dplus1 → NA)
            c = "NA"
            if (index(algo,"cbyd_c") > 0) {
                split(algo,a,"cbyd_c"); c=a[2]+0
                sub("_.*","",c)   # strip trailing _10batch etc
            }
            # strip trailing _10batch for clean algo label kept as-is
            mode = "thread"
            Delta = "NA"
            printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                T,algo,mode,dataset,n,m,Delta,c,m,run,time
        }
    ' >> "$OUT_FILE"
}

# append_c_scaling T
#   Binary CSV: c,num_colors,n,m,Delta,run,time_s
#   mode=c_scaling, algo=cbyd_cC (where C is the c value)
append_c_scaling() {
    local T="$1"
    PARLAY_NUM_THREADS="$T" ./bench_c_scaling \
        --n "$N_FIXED" --runs "$RUNS" \
    | awk -F',' -v T="$T" '
        /^#/ { next }
        {
            c_raw=$1; n=$3; m=$4; Delta=$5; run=$6; time=$7
            # c_raw may be "2", "3", ... or "10b_2", "10b_3", ...
            if (index(c_raw,"10b_") > 0) {
                split(c_raw,a,"10b_"); c=a[2]
                algo="cbyd_10batch"
                mode="c_scaling_10batch"
            } else {
                c=c_raw
                algo="cbyd"
                mode="c_scaling"
            }
            dataset = "rmat_" n
            printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                T,algo,mode,dataset,n,m,Delta,c,m,run,time
        }
    ' >> "$OUT_FILE"
}

# append_dataset_scaling T MODE
#   Binary CSV: mode,algo,dataset,n,m,Delta,batch_edges,run,time_s
append_dataset_scaling() {
    local T="$1" MODE="$2"
    local extra_flags=""
    [[ "$MODE" == "snap" ]] && extra_flags="--snap-dir $SNAP_DIR $HEAVY_FLAG"

    PARLAY_NUM_THREADS="$T" ./bench_dataset_scaling \
        --mode "$MODE" --runs "$RUNS" $extra_flags \
    | awk -F',' -v T="$T" '
        /^#/ { next }
        {
            mode=$1; algo=$2; dataset=$3; n=$4; m=$5; Delta=$6; batch_sz=$7; run=$8; time=$9
            # extract c from algo name (cbyd_c2 → 2, dplus1 → NA)
            c = "NA"
            if (index(algo,"cbyd_c") > 0) {
                split(algo,a,"cbyd_c"); c=a[2]+0
            }
            printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                T,algo,mode,dataset,n,m,Delta,c,batch_sz,run,time
        }
    ' >> "$OUT_FILE"
}

# ── Main sweep loop ───────────────────────────────────────────────────────────
IFS=',' read -ra THREAD_LIST <<< "$THREADS"

for T in "${THREAD_LIST[@]}"; do
    log "===== threads=$T ====="

    log "  thread-scaling workload..."
    append_thread_scaling "$T"

    log "  c-parameter scaling..."
    append_c_scaling "$T"

    log "  dataset scaling: vertex (RMAT n sweep)..."
    append_dataset_scaling "$T" "vertex"

    log "  dataset scaling: batch (batch-size sweep on RMAT 2^18)..."
    append_dataset_scaling "$T" "batch"

    log "  dataset scaling: snap (real-world graphs)..."
    append_dataset_scaling "$T" "snap"

    ROWS=$(wc -l < "$OUT_FILE")
    log "  cumulative rows in CSV: $((ROWS - 1))"
done

# ── Done ─────────────────────────────────────────────────────────────────────
TOTAL=$(( $(wc -l < "$OUT_FILE") - 1 ))
log ""
log "Sweep complete. $TOTAL data rows written to: $OUT_FILE"
log ""

# Quick summary table printed to stderr
log "Per-(threads,algo,mode) average time:"
awk -F',' '
NR == 1 { next }
{
    key = $1 "," $2 "," $3
    sum[key] += $11
    cnt[key] += 1
}
END {
    # header
    printf "%-6s  %-26s  %-15s  %9s\n", "threads", "algo", "mode", "avg_time_s" > "/dev/stderr"
    printf "%-6s  %-26s  %-15s  %9s\n", "------", "--------------------------", "---------------", "----------" > "/dev/stderr"
    n = asorti(sum, sorted_keys)
    for (i = 1; i <= n; i++) {
        k = sorted_keys[i]
        split(k, parts, ",")
        printf "%-6s  %-26s  %-15s  %9.4f\n", parts[1], parts[2], parts[3], sum[k]/cnt[k] > "/dev/stderr"
    }
}
' "$OUT_FILE"
