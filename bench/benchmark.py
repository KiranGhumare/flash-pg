#!/usr/bin/env python3
"""Compare native PostgreSQL, CUDA GFUR and CUDA GFTR in one server session."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import random
import statistics
import subprocess


def custom_nodes(plan):
    found = []
    if plan.get('Custom Plan Provider') == 'GftrAggregate':
        found.append(plan)
    for child in plan.get('Plans', []):
        found.extend(custom_nodes(child))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--psql', default='psql')
    parser.add_argument('--dbname', default='postgres', help='database name or connection URI')
    parser.add_argument('--rows', type=int, default=1000000)
    parser.add_argument('--groups', type=int, default=10000)
    parser.add_argument('--columns', type=int, default=2)
    parser.add_argument('--trials', type=int, default=7)
    parser.add_argument('--device', type=int, default=0)
    parser.add_argument('--max-input-mb', type=int, default=256)
    parser.add_argument('--max-device-mb', type=int, default=4096)
    parser.add_argument('--backend', choices=('cuda', 'cpu'), default='cuda',
                        help='cpu is only a harness smoke test')
    parser.add_argument('--output', type=Path, default=Path('results'))
    args = parser.parse_args()
    if not (1 <= args.rows <= 100000000 and 1 <= args.groups <= args.rows
            and 1 <= args.columns <= 16 and 1 <= args.trials <= 100
            and args.device >= 0 and args.max_input_mb > 0 and args.max_device_mb > 0):
        parser.error('invalid workload size, device, memory budget, or trial count')
    modes = ['native', 'gfur', 'gftr'] if args.backend == 'cuda' else ['native', 'cpu_reference']
    fields = ['k', 'count(*) AS n']
    payloads = []
    for c in range(args.columns):
        name = f'p{c}'
        payloads.append(f"CASE WHEN i % {11 + 2*c} = 0 THEN NULL ELSE "
                        f"((i::bigint * {7919 + 2*c}) % 100001 - 50000)::int END AS {name}")
        fields.extend(f'{op}({name}) AS {op}_{c}' for op in ('count', 'sum', 'min', 'max'))
    query = f"SELECT {', '.join(fields)} FROM gftr_bench GROUP BY k"
    script = [
        'CREATE EXTENSION IF NOT EXISTS pg_gftr;', "LOAD 'pg_gftr';",
        'SET max_parallel_workers_per_gather=0;', 'SET jit=off;',
        f"SET pg_gftr.backend='{args.backend}';", 'SET pg_gftr.force=on;',
        f'SET pg_gftr.device={args.device};',
        f'SET pg_gftr.max_input_mb={args.max_input_mb};',
        f'SET pg_gftr.max_device_mb={args.max_device_mb};',
        f'CREATE TEMP TABLE gftr_bench AS SELECT '
        f'CASE WHEN i % 97 = 0 THEN NULL ELSE '
        f'((hashint8(i::bigint)::bigint & 2147483647) % {args.groups})::int END AS k, '
        f"{', '.join(payloads)} FROM generate_series(1,{args.rows}) AS s(i);",
        'ANALYZE gftr_bench;',
        '\\echo GFTR_RECORD environment',
        "SELECT json_build_object('postgres',version(),'extension',pg_gftr_version(),"
        "'work_mem',current_setting('work_mem'),'hash_mem_multiplier',current_setting('hash_mem_multiplier'));",
    ]

    def configure(mode):
        script.append(f"SET pg_gftr.enabled={'off' if mode == 'native' else 'on'};")
        if mode in ('gftr', 'gfur'):
            script.append(f"SET pg_gftr.mode='{mode}';")

    # Hash result rows in key order. Kept outside all measured executions.
    for mode in modes:
        configure(mode)
        script.extend([
            f'\\echo GFTR_RECORD checksum_{mode}',
            "SELECT md5(string_agg(md5(row_to_json(t)::text),'' ORDER BY k NULLS LAST)) "
            f'FROM ({query}) t;',
        ])
    # One warmup and seven measured trials by default, shuffling mode order.
    rng = random.Random(231200720)
    for trial in range(args.trials + 1):
        order = modes.copy()
        rng.shuffle(order)
        for mode in order:
            configure(mode)
            script.extend([f'\\echo GFTR_RECORD trial_{trial}_{mode}',
                           f'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, FORMAT JSON) {query};'])
    args.output.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    out = args.output / stamp
    out.mkdir()
    (out / 'workload.sql').write_text('\n'.join(script) + '\n')
    process = subprocess.run([args.psql, '-X', '-qAt', '-v', 'ON_ERROR_STOP=1',
                              '--dbname', args.dbname], input='\n'.join(script),
                             capture_output=True, text=True)
    (out / 'stdout.txt').write_text(process.stdout)
    (out / 'stderr.txt').write_text(process.stderr)
    if process.returncode:
        raise SystemExit(f'Benchmark failed; see {out / "stderr.txt"}')
    records = {}
    for block in process.stdout.split('GFTR_RECORD ')[1:]:
        name, value = block.split('\n', 1)
        records[name.strip()] = value.strip()
    hashes = {m: records[f'checksum_{m}'] for m in modes}
    if len(set(hashes.values())) != 1:
        raise SystemExit(f'Result checksums differ: {hashes}; see {out}')
    timings = {m: [] for m in modes}
    plans = []
    for trial in range(args.trials + 1):
        for mode in modes:
            plan = json.loads(records[f'trial_{trial}_{mode}'])[0]
            nodes = custom_nodes(plan['Plan'])
            if bool(nodes) != (mode != 'native'):
                raise SystemExit(f'Unexpected execution plan for {mode}; see {out}')
            if trial:
                timings[mode].append(plan['Execution Time'])
            plans.append({'trial': trial, 'warmup': trial == 0, 'mode': mode, 'explain': plan})
    report = {
        'environment': json.loads(records['environment']),
        'workload': {'rows': args.rows, 'groups_parameter': args.groups, 'columns': args.columns},
        'backend': args.backend, 'checksums': hashes,
        'execution_ms': {m: {'median': statistics.median(v), 'min': min(v), 'max': max(v), 'trials': v}
                         for m, v in timings.items()},
        'plans': plans,
        'measurement': 'EXPLAIN ANALYZE execution time; persistent connection, one warmup; includes row packing and transfers; no normal result delivery',
    }
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    for mode, values in timings.items():
        print(f'{mode}: median {statistics.median(values):.3f} ms')
    print(f'Checksums match. Report: {out / "report.json"}')


if __name__ == '__main__':
    main()
