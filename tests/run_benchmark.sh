#!/bin/bash
# Run Cloud Engine Benchmark
# 
# Usage: ./run_benchmark.sh [engine_path] [model_path]
#
# Prerequisites:
#   pip install psutil

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

ENGINE_PATH="${1:-${PROJECT_ROOT}/build-engine/bin/orca-slice-engine}"
MODEL_PATH="${2:-}"
RESOURCES_PATH="${PROJECT_ROOT}/resources"

# Check engine exists
if [ ! -f "$ENGINE_PATH" ]; then
    echo "ERROR: Engine not found at $ENGINE_PATH"
    echo "Please build first: ./build_slice_engine.sh"
    exit 1
fi

# Check for test model
if [ -z "$MODEL_PATH" ]; then
    # Try to find a test model
    for candidate in \
        "${PROJECT_ROOT}/tests/data/test_model.3mf" \
        "${PROJECT_ROOT}/tests/data/*.3mf" \
        "${PROJECT_ROOT}/resources/test/*.3mf"
    do
        if ls $candidate 1>/dev/null 2>&1; then
            MODEL_PATH=$(ls $candidate | head -1)
            break
        fi
    done
    
    if [ -z "$MODEL_PATH" ]; then
        echo "ERROR: No test model found"
        echo "Please provide a 3MF file: $0 <engine> <model.3mf>"
        exit 1
    fi
fi

echo "=== Cloud Engine Benchmark ==="
echo "Engine: $ENGINE_PATH"
echo "Model: $MODEL_PATH"
echo "Resources: $RESOURCES_PATH"
echo ""

# Check Python dependencies
python3 -c "import psutil" 2>/dev/null || {
    echo "Installing psutil..."
    pip install psutil
}

# Run benchmark
python3 "${SCRIPT_DIR}/cloud_slice_engine_benchmark.py" \
    --engine "$ENGINE_PATH" \
    --model "$MODEL_PATH" \
    --resources "$RESOURCES_PATH" \
    --output "${PROJECT_ROOT}/benchmark_results.json"

echo ""
echo "Results saved to: ${PROJECT_ROOT}/benchmark_results.json"
