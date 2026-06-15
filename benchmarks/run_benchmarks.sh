#!/bin/bash
set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
cd "$SCRIPT_DIR/.."
mkdir -p build && cd build

echo "[System] Disabling ASLR..."
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

echo "[System] Setting CPU governor to performance..."
sudo cpupower frequency-set -g performance

echo "[System] Disabling turbo boost..."
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true

echo "[System] Building benchmarks..."
cmake -DCMAKE_BUILD_TYPE=Release ..
make engine_benchmark -j$(nproc)

echo "[System] Running benchmark on isolated core 3 with RT priority..."
ulimit -l unlimited
sudo chrt -f 99 taskset -c 3 ./benchmarks/engine_benchmark \
    --benchmark_min_time=3s \
    --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true

echo "[System] Restoring system settings..."
echo 1 | sudo tee /proc/sys/kernel/randomize_va_space
echo 0 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true
sudo cpupower frequency-set -g powersave || true