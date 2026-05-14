#!/bin/bash
# Test: verify OrcaSlicer handles port 13619 being occupied (Linux)
# Usage: ./scripts/test_port_fallback_linux.sh [path/to/orca-slicer-binary]

set -e

BINARY="${1:-build/OrcaSlicer_apprel}"

echo "=== Port 13619 Occupancy Test (Linux) ==="
echo ""

# 1. Check port 13619 status before test
echo "[1/4] Checking port 13619..."
if ss -tlnp | grep -q ':13619 ' 2>/dev/null || lsof -i :13619 &>/dev/null; then
    echo "  WARNING: port 13619 is already in use"
else
    echo "  OK: port 13619 is free"
fi

# 2. Occupy port 13619
echo "[2/4] Occupying port 13619 with a dummy listener..."
nc -l 13619 &
NC_PID=$!
sleep 0.5

if ! kill -0 $NC_PID 2>/dev/null; then
    echo "  ERROR: failed to occupy port 13619 (install netcat: sudo apt install netcat-openbsd)"
    exit 1
fi
echo "  OK: port 13619 held by nc (PID $NC_PID)"

# 3. Launch OrcaSlicer
echo "[3/4] Launching OrcaSlicer..."
if [ -f "$BINARY" ]; then
    "$BINARY" &
    ORCA_PID=$!
    echo "  App launched (PID $ORCA_PID). Check logs for:"
else
    echo "  Binary not found at: $BINARY"
    echo "  Usage: $0 [path/to/orca-slicer]"
    kill $NC_PID 2>/dev/null || true
    exit 1
fi
echo "    'Original port 13619 is in use, switching to port 13620'"

# 4. Wait for user to test, then cleanup
echo ""
echo "[4/4] Press Enter after testing to cleanup..."
read -r

kill $NC_PID 2>/dev/null || true
echo "  Port 13619 released."
echo "=== Test complete ==="
