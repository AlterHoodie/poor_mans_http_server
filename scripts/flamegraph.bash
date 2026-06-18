#!/usr/bin/env bash
# Sample a running process by PID and write flamegraph.svg.
#
# Requires: perf, FlameGraph (cloned on first run), root/sudo for perf.
#
# Examples:
#   sudo ./scripts/flamegraph.bash 12345
#   sudo ./scripts/flamegraph.bash --pid 12345 --duration 30 --output /tmp/out.svg
#   sudo ./build/server -l 0 -n 4 -- udp &   # start server yourself
#   sudo ./scripts/flamegraph.bash $!
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$REPO_ROOT/.tools/FlameGraph}"
OUTPUT="$REPO_ROOT/flamegraph.svg"
PID=""
DURATION=15
FREQ=997

usage() {
    cat <<'EOF'
usage: flamegraph.bash PID [options]
       flamegraph.bash --pid PID [options]

Sample a running process and emit a flamegraph SVG.

Options:
  --pid PID            Process to sample (required if not passed as first arg)
  --duration SEC       Sampling window (default: 15)
  --freq HZ            perf sample rate (default: 997)
  --output PATH        Output SVG (default: ./flamegraph.svg)
  -h, --help           Show this help

Environment:
  FLAMEGRAPH_DIR       Path to FlameGraph checkout (cloned on first run if missing)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid)
            PID="$2"
            shift 2
            ;;
        --duration)
            DURATION="$2"
            shift 2
            ;;
        --freq)
            FREQ="$2"
            shift 2
            ;;
        --output)
            OUTPUT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            if [[ -z "$PID" ]]; then
                PID="$1"
                shift
            else
                echo "unknown argument: $1" >&2
                usage >&2
                exit 1
            fi
            ;;
    esac
done

if [[ -z "$PID" ]]; then
    echo "error: PID required" >&2
    usage >&2
    exit 1
fi

if ! [[ "$PID" =~ ^[0-9]+$ ]]; then
    echo "error: PID must be a number (got: $PID)" >&2
    exit 1
fi

if ! command -v perf >/dev/null 2>&1; then
    echo "error: perf not found" >&2
    exit 1
fi

if ! kill -0 "$PID" 2>/dev/null; then
    echo "error: no process with pid $PID" >&2
    exit 1
fi

run_root() {
    if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
        sudo "$@"
    else
        "$@"
    fi
}

ensure_flamegraph() {
    if [[ -x "$FLAMEGRAPH_DIR/flamegraph.pl" && -x "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" ]]; then
        return
    fi
    if ! command -v git >/dev/null 2>&1; then
        echo "error: git required to clone FlameGraph into $FLAMEGRAPH_DIR" >&2
        exit 1
    fi
    echo "Cloning FlameGraph into $FLAMEGRAPH_DIR ..."
    mkdir -p "$(dirname "$FLAMEGRAPH_DIR")"
    git clone --depth 1 https://github.com/brendangregg/FlameGraph.git "$FLAMEGRAPH_DIR"
}

ensure_flamegraph

PERF_DATA="$(mktemp /tmp/poor-mans-perf-XXXXXX.data)"
trap 'run_root rm -f "$PERF_DATA"' EXIT

echo "Recording pid=$PID for ${DURATION}s at ${FREQ}Hz ..."
run_root perf record \
    -F "$FREQ" \
    -g \
    --call-graph fp \
    -p "$PID" \
    -o "$PERF_DATA" \
    -- sleep "$DURATION"

echo "Rendering flamegraph ..."
run_root perf script -i "$PERF_DATA" \
    | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" \
    | "$FLAMEGRAPH_DIR/flamegraph.pl" \
        --title "pid $PID (${DURATION}s)" \
        --width 1200 \
    > "$OUTPUT"

echo "Wrote $OUTPUT"
