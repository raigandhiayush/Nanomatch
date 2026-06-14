#!/bin/bash
set -e

# Get the directory where this script actually lives (benchmarks/)
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

# Move to the root Nanomatch folder and create build right there
cd "$SCRIPT_DIR/.."
mkdir -p build
cd build

echo "[System] Setting CPU Governor to high-performance mode..."
sudo cpupower frequency-set -g performance || echo "Warning: Could not set CPU governor."

echo "[System] Configuring and compiling benchmarks..."
cmake -DCMAKE_BUILD_TYPE=Release ..
make engine_benchmark

echo "[System] Running Google Benchmark pinned strictly to CPU Core 1..."
taskset -c 1 ./benchmarks/engine_benchmark

echo "[System] Resetting CPU Governor back to powersave..."
sudo cpupower frequency-set -g powersave || true