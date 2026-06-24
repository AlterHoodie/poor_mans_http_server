#!/usr/bin/env bash
# Record perf for an already-running process, then write flamegraph.svg.
#
# Usage:
#   ./scripts/profile_server.sh <PID> -d 30
#   ./scripts/profile_server.sh 12345 -d 30 -o profiles/my_run

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PID=""
DURATION=30
OUTPUT_DIR=""
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$HOME/Flamegraph}"

usage() {
    cat <<'EOF'
Record perf for a running process, then generate flamegraph.svg.

Usage:
  ./scripts/profile_server.sh <PID> [options]

Options:
  -d, --duration SECS     Record duration (default: 30)
  -o, --output DIR        Output directory (default: profiles/run_<timestamp>)
      --flamegraph-dir DIR  FlameGraph repo (default: ~/Flamegraph)
  -h, --help              Show this help
EOF
}

log() { printf '[profile] %s\n' "$*"; }
die() { printf '[profile] error: %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--duration) DURATION="$2"; shift 2 ;;
        -o|--output) OUTPUT_DIR="$2"; shift 2 ;;
        --flamegraph-dir) FLAMEGRAPH_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option: $1" ;;
        *)
            [[ -z "$PID" ]] || die "unexpected argument: $1"
            PID="$1"
            shift
            ;;
    esac
done

[[ -n "$PID" ]] || { usage; die "PID required"; }
[[ "$PID" =~ ^[0-9]+$ ]] || die "PID must be a number"
[[ "$DURATION" =~ ^[0-9]+$ && "$DURATION" -gt 0 ]] || die "duration must be a positive integer"
kill -0 "$PID" 2>/dev/null || die "no process with PID $PID"

command -v perf >/dev/null 2>&1 || die "missing command: perf"

if [[ -f "$FLAMEGRAPH_DIR" && "$FLAMEGRAPH_DIR" == *.pl ]]; then
    FLAMEGRAPH_DIR="$(dirname "$FLAMEGRAPH_DIR")"
fi

STACKCOLLAPSE="$FLAMEGRAPH_DIR/stackcollapse-perf.pl"
FLAMEGRAPH="$FLAMEGRAPH_DIR/flamegraph.pl"
[[ -f "$STACKCOLLAPSE" ]] || die "missing $STACKCOLLAPSE"
[[ -f "$FLAMEGRAPH" ]] || die "missing $FLAMEGRAPH"

if [[ -z "$OUTPUT_DIR" ]]; then
    OUTPUT_DIR="$ROOT/profiles/run_$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "$OUTPUT_DIR"

PERF_DATA="$OUTPUT_DIR/perf.data"
FLAMEGRAPH_SVG="$OUTPUT_DIR/flamegraph.svg"

log "pid: $PID"
log "duration: ${DURATION}s"
log "output: $OUTPUT_DIR"

if ! perf record -F 99 -g -p "$PID" -o "$PERF_DATA" -- sleep "$DURATION" 2>"$OUTPUT_DIR/perf.stderr"; then
    if [[ "$(id -u)" -ne 0 ]]; then
        log "retrying with sudo"
        sudo perf record -F 99 -g -p "$PID" -o "$PERF_DATA" -- sleep "$DURATION" 2>"$OUTPUT_DIR/perf.stderr"
    else
        die "perf record failed (see $OUTPUT_DIR/perf.stderr)"
    fi
fi

log "generating flamegraph -> $FLAMEGRAPH_SVG"
perf script -i "$PERF_DATA" 2>/dev/null \
    | perl "$STACKCOLLAPSE" \
    | perl "$FLAMEGRAPH" --title "PID $PID (${DURATION}s)" >"$FLAMEGRAPH_SVG"

log "done"
log "  flamegraph: $FLAMEGRAPH_SVG"
log "  perf data:  $PERF_DATA"
