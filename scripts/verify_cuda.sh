#!/usr/bin/env bash
# Run as an ordinary user. Installation must already be writable to that user.
set -euo pipefail
cd "$(dirname "$0")/.."
export PG_CONFIG="${PG_CONFIG:-pg_config}"
export CUDA_HOME="${CUDA_HOME:-/usr/local/cuda}"
export CUDA_ARCH="${CUDA_ARCH:-86}"
"$PG_CONFIG" --version
"$CUDA_HOME/bin/nvcc" --version
nvidia-smi
make PG_CONFIG="$PG_CONFIG" USE_CUDA=1 clean
make PG_CONFIG="$PG_CONFIG" USE_CUDA=1 CUDA_HOME="$CUDA_HOME" CUDA_ARCH="$CUDA_ARCH" -j"${JOBS:-4}"
make PG_CONFIG="$PG_CONFIG" test-cpu
make PG_CONFIG="$PG_CONFIG" USE_CUDA=1 CUDA_HOME="$CUDA_HOME" CUDA_ARCH="$CUDA_ARCH" test-cuda
make PG_CONFIG="$PG_CONFIG" USE_CUDA=1 CUDA_HOME="$CUDA_HOME" install
python3 scripts/test_postgres.py --pg-config "$PG_CONFIG" --backend cuda --benchmark-smoke --results-dir results/verification
printf 'CUDA sm_%s build and correctness checks passed.\n' "$CUDA_ARCH"
