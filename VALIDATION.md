# Validation recorded October 24, 2026

## Executed locally

Environment: Apple M4 / arm64 macOS, PostgreSQL 18.6 built from the user's
existing source, CPU-only extension build. A separate PostgreSQL installation
under `/private/tmp/pg-gftr-pg18` and disposable test clusters were used.
Existing database clusters were not accessed or changed.

Passed:

1. PGXS build and installation of the PostgreSQL extension, without compiler
   warnings in the final build.
2. Standalone CPU backend tests for NULL keys/values, signed extremes, all-NULL
   payload groups, multiple columns, empty input and count-star-only input.
3. The same standalone tests with AddressSanitizer and UndefinedBehaviorSanitizer.
4. PostgreSQL integration tests: 24 result/type/plan comparisons with native
   execution, plus prepared statements, an explicitly scrollable cursor,
   input-memory budget failure, error recovery, and rejection of CUDA execution
   when the CUDA backend has not been built. Correlated subqueries exercise
   rescans with sequential and indexed child plans.
5. Benchmark harness smoke test: 20,000 rows, 37 requested non-NULL groups,
   two payload columns, native PostgreSQL versus the CPU reference, one warmup
   and two measured rounds. Result checksums matched and expected plan providers
   were verified. This run is a harness check, not a GPU performance result.
6. Python syntax checks for both scripts and Bash syntax check for the A100
   verification script.

Main integration command:

```bash
DYLD_LIBRARY_PATH=/private/tmp/pg-gftr-pg18/lib \
python3 scripts/test_postgres.py \
  --pg-config /private/tmp/pg-gftr-pg18/bin/pg_config \
  --backend cpu --benchmark-smoke
```

The library-path override is specific to relocating this existing macOS
PostgreSQL build. It is not a required Linux setup step.

## Executed on the user's RTX A6000 Linux instance

Connected through `tnr connect 0` with the user's authorization. Environment:

- NVIDIA RTX A6000, 49,140 MiB reported device memory, driver 610.43.02.
- Ubuntu 22.04.5 LTS x86_64, GCC 11.4.0.
- CUDA Toolkit 12.6, nvcc 12.6.85, CUB from CUDA CCCL 12.6.77.
- PostgreSQL 18.6, built with `-O2 -g`, assertions enabled, ICU disabled.
- Extension built for `sm_86` plus `compute_86` PTX.

The CUDA compiler/runtime/CCCL redistributions were downloaded from NVIDIA
and verified against the SHA256 hashes in its 12.6.3 redistribution manifest.
PostgreSQL 18.6 source was verified against its published SHA256 checksum.
Only build dependencies (Bison, Flex, readline/zlib development packages) were
installed through the OS package manager; no GPU driver or system PostgreSQL
service was installed or modified.

Passed on the final source:

1. CUDA extension compilation and PGXS installation, without compiler warnings.
2. Standalone CPU edge-case tests.
3. All 72 standalone fixtures under GFTR and GFUR (144 GPU executions), compared
   with the CPU oracle, including empty input, NULLs, multiple payload columns,
   extreme integer values, varied group cardinality and skewed groups. The
   too-small device memory budget test also passed.
4. The complete PostgreSQL SQL integration suite under **each** CUDA algorithm:
   24 result/type/plan comparisons per algorithm, plus cached statements,
   correlated indexed rescans, cursor fetching, memory-limit errors and recovery.
5. The native/GFUR/GFTR benchmark harness smoke test, including result checksums
   and execution-plan verification. Temporary test clusters were stopped and
   removed; logs and JSON reports were retained.

Smoke workload: 20,000 rows, 37 requested non-NULL groups, two payload columns,
one warmup and two measured rounds, serial PostgreSQL with JIT disabled.
Observed median instrumented query execution times were native **10.250 ms**,
GFUR **20.550 ms**, and GFTR **22.684 ms**. Native PostgreSQL was faster on this
small fixture. These numbers validate the harness, not the paper's performance
claims or an A100 speedup; larger workload sweeps and GPU-specific cost tuning
remain necessary.

Remote files:

```text
/home/ubuntu/pg_gftr/                            extension source
/home/ubuntu/pg-gftr-tools/postgres-18/           PostgreSQL installation
/home/ubuntu/pg-gftr-tools/cuda-12.6/             CUDA development tools
/home/ubuntu/pg_gftr/results/verification/        SQL logs and benchmark reports
/home/ubuntu/pg-gftr-tools/verification-a6000.log complete build/test log
```

Rerun there:

```bash
cd /home/ubuntu/pg_gftr
PG_CONFIG=/home/ubuntu/pg-gftr-tools/postgres-18/bin/pg_config \
CUDA_HOME=/home/ubuntu/pg-gftr-tools/cuda-12.6 \
bash scripts/verify_a6000.sh
```

A100 execution remains untested. For an A100, use `verify_a100.sh` (sm_80).
The code is still the first aggregation milestone, not a full paper
reproduction; GPU joins and the other algorithms listed in README are absent.
