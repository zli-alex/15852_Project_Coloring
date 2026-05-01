#!/usr/bin/env bash
# run_benchmarks.sh
#
# Orchestrates building and running all benchmark binaries, then collects
# results into CSV files under results/.
#
# Usage:
#   ./run_benchmarks.sh [OPTIONS]
#
# Options:
#   --snap-dir DIR   Path to SNAP data directory   (default: ./data)
#   --runs N         Timed repetitions per point    (default: 5)
#   --n N            RMAT vertex count for thread/c scaling (default: 262144 = 2^18)
#   --threads LIST   Comma-separated thread counts  (default: 1,2,4,8,16,32)
#   --heavy          Include com-youtube in SNAP tests
#   --skip-build     Skip `make bench` step
#   --help           Show this help text

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────────────
SNAP_DIR="data"
RUNS=5
N_VERTICES=262144        # 2^18
THREAD_LIST="1,2,4,8,16,32"
HEAVY_FLAG=""
SKIP_BUILD=0
RESULTS_DIR="results"

# ── Argument parsing ───────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --snap-dir)   SNAP_DIR="$2";      shift 2 ;;
        --runs)       RUNS="$2";          shift 2 ;;
        --n)          N_VERTICES="$2";    shift 2 ;;
        --threads)    THREAD_LIST="$2";   shift 2 ;;
        --heavy)      HEAVY_FLAG="--heavy"; shift ;;
        --skip-build) SKIP_BUILD=1;       shift ;;
        --help)
            grep '^#' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# ── Setup ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

mkdir -p "$RESULTS_DIR"

log() { echo "[run_benchmarks] $*"; }

# ── Build ──────────────────────────────────────────────────────────────────────
if [[ $SKIP_BUILD -eq 0 ]]; then
    log "Building benchmark binaries..."
    make bench
    log "Build complete."
fi

# ── 1. Thread / speedup scaling ──────────────────────────────────────────────
THREAD_CSV="${RESULTS_DIR}/thread_scaling.csv"
log "Running thread-scaling benchmark → ${THREAD_CSV}"

./bench_thread_scaling --header > "$THREAD_CSV"

IFS=',' read -ra THREADS <<< "$THREAD_LIST"
for T in "${THREADS[@]}"; do
    log "  PARLAY_NUM_THREADS=$T"
    PARLAY_NUM_THREADS="$T" ./bench_thread_scaling \
        --n "$N_VERTICES" --runs "$RUNS" >> "$THREAD_CSV"
done

log "Thread-scaling done. Rows written: $(tail -n +2 "$THREAD_CSV" | wc -l)"

# ── 2. c-parameter scaling (cbyd) ─────────────────────────────────────────────
C_CSV="${RESULTS_DIR}/c_scaling.csv"
log "Running c-parameter scaling benchmark → ${C_CSV}"

./bench_c_scaling --header > "$C_CSV"
./bench_c_scaling --n "$N_VERTICES" --runs "$RUNS" >> "$C_CSV"

log "c-scaling done. Rows written: $(tail -n +2 "$C_CSV" | wc -l)"

# ── 3. Dataset / vertex-count scaling ─────────────────────────────────────────
DS_CSV="${RESULTS_DIR}/dataset_scaling.csv"
log "Running dataset-scaling benchmark → ${DS_CSV}"

./bench_dataset_scaling --header > "$DS_CSV"

log "  mode=vertex (RMAT n scaling)..."
./bench_dataset_scaling --mode vertex \
    --runs "$RUNS" $HEAVY_FLAG >> "$DS_CSV"

log "  mode=batch (batch-size scaling on RMAT 2^18)..."
./bench_dataset_scaling --mode batch \
    --runs "$RUNS" $HEAVY_FLAG >> "$DS_CSV"

log "  mode=snap (real-world SNAP datasets)..."
./bench_dataset_scaling --mode snap \
    --snap-dir "$SNAP_DIR" --runs "$RUNS" $HEAVY_FLAG >> "$DS_CSV"

log "Dataset-scaling done. Rows written: $(tail -n +2 "$DS_CSV" | wc -l)"

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "========================================================"
echo "  All benchmarks complete. Results in: ${RESULTS_DIR}/"
echo "  $(ls -lh "${RESULTS_DIR}"/*.csv 2>/dev/null | awk '{print $5, $9}' | xargs)"
echo "========================================================"

# ── Optional: print per-algo speedup table from thread_scaling.csv ────────────
echo ""
echo "Thread-scaling summary (avg time per algo):"
echo "algo                 | T=1     | T=max   | speedup"
echo "---------------------+---------+---------+--------"

# Use awk to compute per-algo mean at T=1 and T=max, print speedup ratio.
awk -F',' -v max_t="$(echo "$THREAD_LIST" | tr ',' '\n' | tail -1)" '
NR == 1 { next }   # skip header
{
    algo = $1; T = $2; t = $7
    sum[algo][T] += t
    cnt[algo][T] += 1
}
END {
    for (algo in sum) {
        mean1   = (cnt[algo][1]     > 0) ? sum[algo][1]     / cnt[algo][1]     : 0
        meanmax = (cnt[algo][max_t] > 0) ? sum[algo][max_t] / cnt[algo][max_t] : 0
        speedup = (meanmax > 0) ? mean1 / meanmax : 0
        printf "%-20s | %7.3f | %7.3f | %6.2f\n", algo, mean1, meanmax, speedup
    }
}
' "$THREAD_CSV" | sort
