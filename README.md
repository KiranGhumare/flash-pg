# pg_gftr

An experimental PostgreSQL **18** extension implementing GPU sort-based
grouped aggregation with GFTR (gather from transformed relations). PostgreSQL
core does not need patches. Build targets include RTX A6000 (**sm_86**) and
A100 (**sm_80**).

This is the first aggregation milestone from *Efficiently Processing Joins and
Grouped Aggregations on GPUs*, Wu et al., SIGMOD 2025,
[DOI: 10.1145/3709689](https://doi.org/10.1145/3709689), especially sections
4.1.2–4.1.3. It is an independent implementation, not the authors' code or a
reproduction of all their experiments.

## Implemented scope

- Ordinary SQL `GROUP BY` on one plain `integer` (`int4`) column.
- Built-in `COUNT(*)`, `COUNT(int4)`, `SUM(int4)`, `MIN(int4)`, `MAX(int4)`.
- Multiple integer payload columns; repeated aggregates share column results.
- NULL grouping keys, NULL payloads, signed integer extremes, empty input,
  `SUM(int4)` / `COUNT` returning `bigint`, `MIN` / `MAX` returning `integer`.
- PostgreSQL child plans perform scans, filtering and any CPU joins under the
  normal query snapshot. The extension aggregates their output.
- Custom upper paths, `CustomScan`, rescans, parameter propagation, cleanup
  on query abort, and `EXPLAIN ANALYZE` instrumentation.
- Two CUDA modes: GFTR sorts actual payloads alongside keys; GFUR sorts tuple
  IDs and reads original payloads through the resulting map.
- Explicit CPU reference backend for local correctness testing.

GPU joins, hash/partitioned aggregation, dictionary encoding, multiple group
keys, expression group keys/aggregate arguments, `bigint`/numeric/text keys or
payloads, global aggregation without GROUP BY, HAVING, FILTER, DISTINCT
aggregates, ordered aggregates, grouping sets, window queries and partitionwise
aggregation are outside this version. Unsupported shapes keep native paths.
Output expressions such as `sum(a)+1` are conservatively declined.

The custom operator is serial from PostgreSQL's perspective. It may consume a
native parallel child plan, but it does not itself implement parallel workers,
partial/final aggregation, disk spilling or multiple GPUs.

## Build on an RTX A6000 or A100 Linux machine

Requirements: PostgreSQL 18 server headers and PGXS, a compatible C/C++ host
compiler, CUDA Toolkit 12.2 or later with CUB, and a compatible NVIDIA driver.
Python 3 is needed for test/benchmark scripts. No Python packages are required.
Build and run PostgreSQL **on the NVIDIA machine**, not on the Mac with GPU
calls over SSH.

Copy this folder to the machine, then use the `pg_config` belonging to the
server you intend to run:

```bash
cd ~/pg_gftr
export PG_CONFIG="$HOME/pg18/bin/pg_config" # change to your installation
export CUDA_HOME=/usr/local/cuda           # change for your CUDA module
export PATH="$CUDA_HOME/bin:$PATH"

"$PG_CONFIG" --version                    # must say PostgreSQL 18.x
nvidia-smi
bash scripts/verify_a6000.sh              # RTX A6000, sm_86
# bash scripts/verify_a100.sh             # A100, sm_80
```

The verification script cleans stale objects, builds for the selected GPU, runs CPU tests,
compares both CUDA algorithms against the CPU oracle across 72 fixtures (144
GPU runs), installs the extension, and runs SQL correctness tests in disposable
PostgreSQL clusters under both CUDA modes, followed by a benchmark harness
smoke test. Logs and reports are retained under `results/verification`; the
temporary clusters are stopped and removed. Use a user-owned PostgreSQL
installation so `make install` is writable. `initdb` must run as a non-root
user. It opens only a unique local Unix socket; existing servers are untouched.
No performance claim should be made until these CUDA checks pass on your GPU.

Individual build commands:

```bash
make PG_CONFIG="$PG_CONFIG" USE_CUDA=1 clean
make PG_CONFIG="$PG_CONFIG" USE_CUDA=1 CUDA_ARCH=86 -j4
make PG_CONFIG="$PG_CONFIG" USE_CUDA=1 install
make PG_CONFIG="$PG_CONFIG" USE_CUDA=1 CUDA_ARCH=86 test-cuda
python3 scripts/test_postgres.py --pg-config "$PG_CONFIG" --backend cuda
```

`CUDA_HOME`, `NVCC`, `NVCCFLAGS` and `CUDA_ARCH` can be overridden on make's
command line. Clean before switching between CPU-only and CUDA builds.
The makefile links the CUDA runtime with a runtime search path to
`$CUDA_HOME/lib64`. That location must be accessible to the PostgreSQL server.
With module-based CUDA installations, load the module before building and
starting the server. `CUDA_VISIBLE_DEVICES` controls which devices PostgreSQL
can see; `pg_gftr.device` selects an ordinal within that visible set.

## Use in SQL

Run as a PostgreSQL superuser for installation and `LOAD`:

```sql
CREATE EXTENSION pg_gftr;
LOAD 'pg_gftr';
SET pg_gftr.enabled = on;
SET pg_gftr.backend = 'cuda';
SET pg_gftr.mode = 'gftr';
SET pg_gftr.force = on;  -- experimental override for supported queries

EXPLAIN (ANALYZE, BUFFERS)
SELECT k, count(*), count(a), sum(a), min(b), max(b)
FROM your_table
GROUP BY k;
```

Expect `Custom Scan (GftrAggregate)` with `Backend: CUDA`, algorithm, device,
input/group counts, scan-and-pack time, backend wall time, and GPU phase times.
`sql/demo.sql` creates a temporary million-row fixture and runs GFTR, GFUR and
native plans:

```bash
"$("$PG_CONFIG" --bindir)/psql" -X -d your_database -f sql/demo.sql
```

`CREATE EXTENSION` loads hooks in its creating session. **Every new session
must `LOAD 'pg_gftr'`**, call `SELECT pg_gftr_version()`, or use an administrator
configured preload library. No server restart is needed for session `LOAD`.
Installing extension SQL objects alone does not load hooks into other sessions.
`enabled` defaults to off. `force` defaults to off. There is no silent CPU
fallback when a selected CUDA path fails.

## Planner and memory controls

| Setting | Default | Meaning |
|---|---:|---|
| `pg_gftr.enabled` | off | Offer custom paths for supported queries |
| `pg_gftr.force` | off | Give supported custom paths an artificially low cost |
| `pg_gftr.backend` | cuda | `cuda` or explicit `cpu` reference |
| `pg_gftr.mode` | gftr | `gftr` or `gfur`; CPU backend ignores this setting |
| `pg_gftr.device` | 0 | Visible CUDA device ordinal |
| `pg_gftr.min_rows` | 100000 | Estimated input threshold; bypassed by force |
| `pg_gftr.max_input_mb` | 256 | Packed input capacity limit, in MiB |
| `pg_gftr.max_device_mb` | 4096 | Per-operator device allocation limit, in MiB |
| `pg_gftr.setup_cost` | 1000 | Provisional GPU setup cost in planner units |
| `pg_gftr.tuple_cost` | 0.02 | Provisional input cost per tuple |

The cost model includes the child plan's total cost and a tunable input cost.
It has **not been calibrated on GPU hardware**. Use `force=on` for controlled
comparisons, then calibrate costs before relying on automatic selection. The
override does not enable unsupported SQL. `enabled`/`force` affect planning;
reprepare cached statements after changing them. Backend/mode/device/memory
settings are captured when an executor starts.

This version buffers the complete input. Host rows take 8 bytes for an encoded
key plus 8 bytes per distinct referenced payload column. Host results, the CPU
sort workspace, PostgreSQL child memory and allocation overhead are **additional
to** `max_input_mb`. Device buffers and CUB scratch count toward
`max_device_mb`, further capped at 90% of currently free device memory. No memory
reservation is coordinated between database sessions. If input or device
capacity is exceeded, the statement errors; disable the extension for that
statement or increase the relevant budget. No partial results are emitted.
Query memory cleanup releases host results after errors; C++ RAII releases
device allocations before returning failures to PostgreSQL.

## Algorithm and measurements

Input int4 keys are mapped to unsigned order with a separate NULL sentinel;
CUB sorts 33 meaningful bits. GFTR repeatedly uses stable `SortPairs` on the
**original keys and actual payload column**, preserving column alignment.
It never implements transformation by gathering original values through
sorted IDs. GFUR sorts `(key, row ID)` once and uses indirect payload reads.
Run-length encoding identifies groups, an exclusive scan locates their
boundaries, and a block reduction computes column statistics for each group.
With count-star only, both modes sort keys without payload columns.

The initial kernel uses one CUDA block per group, with a grid-stride loop over
groups. Extremely small groups can waste threads; this is a clear tuning target.
Computing count/sum/min/max together can also do work the SQL query does not
request. This is a research baseline, not an optimized replacement for every
PostgreSQL aggregate.

`Scan and Pack` includes the child plan's execution and row conversion.
`Backend Total` is host wall time including device setup, allocations,
synchronization, work and cleanup. `Sort`, `Group and Reduce`, `Transfer` are
CUDA-event measurements on a dedicated stream; their sum excludes some host
overhead. Backend phase times describe the latest execution if a node is
rescanned, while PostgreSQL's standard timing/loops describe all executions.
Cancellation is checked during row packing and after the synchronous backend
call; an in-flight sort/reduction is not immediately interruptible.

The paper's main operator results assume GPU-resident data. This extension's
complete queries include conversion and transfer costs, so kernel speedups
must not be presented as complete-query speedups.

## Reproducible query benchmark

```bash
python3 bench/benchmark.py \
  --psql "$("$PG_CONFIG" --bindir)/psql" \
  --dbname your_database --rows 1000000 --groups 10000 --columns 2 --trials 7
```

It creates only a temporary fixture (plus `CREATE EXTENSION IF NOT EXISTS`),
uses one connection, checks result hashes outside measured executions, warms
each mode, and shuffles seven measured rounds of native/GFUR/GFTR. Reports save
SQL, environment, complete JSON plans, and execution-time medians/min/max.
Native plans must lack the custom operator; GPU plans must contain it. Results
are instrumented `EXPLAIN ANALYZE` execution times, excluding ordinary result
delivery. Parallel query and JIT are disabled for this controlled baseline;
the report records `work_mem` and `hash_mem_multiplier`. Sweep group cardinality,
payload width and input size rather than relying on one workload.

## Local CPU verification

```bash
make PG_CONFIG=/path/to/pg18/bin/pg_config USE_CUDA=0 clean
make PG_CONFIG=/path/to/pg18/bin/pg_config USE_CUDA=0 -j4
make PG_CONFIG=/path/to/pg18/bin/pg_config USE_CUDA=0 install
make PG_CONFIG=/path/to/pg18/bin/pg_config test-cpu
python3 scripts/test_postgres.py --pg-config /path/to/pg18/bin/pg_config --backend cpu
```

CPU-only builds reject CUDA execution with a rebuild instruction. They do not
pretend to run on the GPU. The SQL suite checks result multisets and types
against native execution, planner eligibility, NULLs, extremes, empty input,
filters, native joins as children, correlated rescans including index scans,
prepared statements, cursor fetching, memory-limit errors and recovery.

See [VALIDATION.md](VALIDATION.md) for what was actually run during creation.

## Source map

- `src/pg_gftr.c`: GUCs, eligibility, upper path, plan and executor callbacks.
- `src/gftr_backend.h`: C ABI shared by PostgreSQL, CPU and CUDA code.
- `src/gftr_cuda.cu`: CUB primitives, CUDA reduction, budgets and RAII cleanup.
- `src/gftr_cpu.c`: CPU correctness oracle and host result ownership.
- `scripts/verify_cuda.sh`: shared CUDA build and correctness checks.
- `scripts/verify_a6000.sh`, `scripts/verify_a100.sh`: GPU-specific build targets.
- `tests/`: standalone and SQL integration fixtures.
- `bench/benchmark.py`: native/GFUR/GFTR query measurements.

The integration uses PostgreSQL internal APIs, so rebuild for each supported
major version; other major versions are currently rejected at compile time.
References: [custom scan paths](https://www.postgresql.org/docs/18/custom-scan-path.html),
[custom scan execution](https://www.postgresql.org/docs/18/custom-scan-execution.html),
[PGXS](https://www.postgresql.org/docs/18/extend-pgxs.html), and
[NVIDIA CUB](https://github.com/NVIDIA/cccl/tree/main/cub).
