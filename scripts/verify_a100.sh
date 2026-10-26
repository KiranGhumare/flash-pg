#!/usr/bin/env bash
set -euo pipefail
export CUDA_ARCH="${CUDA_ARCH:-80}"
exec bash "$(dirname "$0")/verify_cuda.sh"
