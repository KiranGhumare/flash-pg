\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pg_gftr;
LOAD 'pg_gftr';
SET pg_gftr.enabled = on;
SET pg_gftr.backend = 'cuda';
SET pg_gftr.mode = 'gftr';
SET pg_gftr.force = on;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TEMP TABLE gftr_demo AS
SELECT (i % 4096)::int AS k,
       CASE WHEN i % 11 = 0 THEN NULL ELSE (i % 1000)::int END AS a,
       ((i::bigint * 17) % 2000)::int AS b
FROM generate_series(1, 1000000) i;
ANALYZE gftr_demo;

EXPLAIN (ANALYZE, BUFFERS)
SELECT k, count(*), count(a), sum(a), min(b), max(b)
FROM gftr_demo GROUP BY k;

SET pg_gftr.mode = 'gfur';
EXPLAIN (ANALYZE, BUFFERS)
SELECT k, count(*), count(a), sum(a), min(b), max(b)
FROM gftr_demo GROUP BY k;

SET pg_gftr.enabled = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT k, count(*), count(a), sum(a), min(b), max(b)
FROM gftr_demo GROUP BY k;
