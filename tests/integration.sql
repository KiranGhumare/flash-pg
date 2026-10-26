\set ON_ERROR_STOP on
CREATE EXTENSION pg_gftr;
SET pg_gftr.backend = :'backend';
SET pg_gftr.force = on;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TEMP TABLE fixture(k int, a int, b int);
INSERT INTO fixture VALUES
 (NULL, NULL, 3), (NULL, 4, NULL),
 (-2147483648, -2147483648, 1), (-2147483648, 2147483647, 2),
 (2147483647, NULL, 4), (2147483647, NULL, 5),
 (0, -4, NULL), (0, 7, NULL), (1, 8, 9), (1, 8, 9);
INSERT INTO fixture
SELECT CASE WHEN i % 37 = 0 THEN NULL ELSE (i * 17 % 2048) - 1024 END,
       CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 97 % 10001) - 5000 END,
       CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 43 % 101) - 50 END
FROM generate_series(1, 20000) i;
ANALYZE fixture;

CREATE FUNCTION pg_temp.assert_same(q text, custom_expected boolean)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE
    p json;
    mismatch boolean;
    differences json;
    actual_sample json;
BEGIN
    PERFORM set_config('pg_gftr.enabled', 'off', true);
    EXECUTE 'CREATE TEMP TABLE expected_result AS ' || q;
    PERFORM set_config('pg_gftr.enabled', 'on', true);
    EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO p;
    IF (p::text LIKE '%GftrAggregate%') <> custom_expected THEN
        RAISE EXCEPTION 'unexpected plan for %: %', q, p;
    END IF;
    EXECUTE 'CREATE TEMP TABLE actual_result AS ' || q;
    SELECT EXISTS (
        (SELECT * FROM expected_result EXCEPT ALL SELECT * FROM actual_result)
        UNION ALL
        (SELECT * FROM actual_result EXCEPT ALL SELECT * FROM expected_result)
    ) INTO mismatch;
    IF mismatch THEN
        SELECT json_agg(d) INTO differences FROM
          (SELECT * FROM expected_result EXCEPT ALL SELECT * FROM actual_result LIMIT 5) d;
        SELECT json_agg(d) INTO actual_sample FROM (SELECT * FROM actual_result LIMIT 5) d;
        RAISE EXCEPTION 'result mismatch: %; missing rows: %; actual sample: %', q, differences, actual_sample;
    END IF;
    IF EXISTS (
        SELECT 1 FROM pg_attribute e JOIN pg_attribute a USING(attnum)
        WHERE e.attrelid = 'expected_result'::regclass
          AND a.attrelid = 'actual_result'::regclass AND e.attnum > 0
          AND NOT e.attisdropped AND e.atttypid <> a.atttypid
    ) THEN RAISE EXCEPTION 'return type mismatch: %', q; END IF;
    DROP TABLE expected_result, actual_result;
END $$;

SELECT pg_temp.assert_same(
 'SELECT k, count(*) AS n, count(a) AS na, sum(a) AS sa, min(a) AS la, max(a) AS ha, count(b) AS nb, sum(b) AS sb, min(b) AS lb, max(b) AS hb FROM fixture GROUP BY k', true);
SELECT pg_temp.assert_same('SELECT count(*) FROM fixture GROUP BY k', true);
SELECT pg_temp.assert_same('SELECT sum(a), k, max(b), sum(a) AS duplicate FROM fixture GROUP BY k', true);
SELECT pg_temp.assert_same('SELECT k, count(k), sum(k), min(k), max(k) FROM fixture GROUP BY k ORDER BY k NULLS FIRST', true);
SELECT pg_temp.assert_same('SELECT k, count(*), sum(a) FROM fixture WHERE a < 0 GROUP BY k', true);
SELECT pg_temp.assert_same('SELECT k, count(*), sum(a) FROM fixture WHERE false GROUP BY k', true);
SELECT pg_temp.assert_same('SELECT k, min(a), max(a) FROM fixture GROUP BY k ORDER BY min(a), k LIMIT 7', true);
SELECT pg_temp.assert_same('SELECT k, count(*) FROM fixture WHERE k IS NULL GROUP BY k', true);
SELECT pg_temp.assert_same('SELECT k, count(*) FROM fixture GROUP BY k HAVING count(*) > 3', false);
SELECT pg_temp.assert_same('SELECT k, avg(a) FROM fixture GROUP BY k', false);
SELECT pg_temp.assert_same('SELECT k, count(DISTINCT a) FROM fixture GROUP BY k', false);
SELECT pg_temp.assert_same('SELECT k, sum(a) FILTER (WHERE a > 0) FROM fixture GROUP BY k', false);
SELECT pg_temp.assert_same('SELECT k, b, count(*) FROM fixture GROUP BY k,b', false);
SELECT pg_temp.assert_same('SELECT k::bigint, sum(a) FROM fixture GROUP BY k::bigint', false);
SELECT pg_temp.assert_same('SELECT k+1, count(*) FROM fixture WHERE k < 2147483647 GROUP BY k+1', false);
SELECT pg_temp.assert_same('SELECT sum(a) FROM fixture', false);
SELECT pg_temp.assert_same('SELECT k, sum(a)+1 AS x FROM fixture GROUP BY k', false);
SELECT pg_temp.assert_same('SELECT k, count(*) FILTER (WHERE a IS NULL) FROM fixture GROUP BY k', false);
SELECT pg_temp.assert_same('SELECT k, count(*) FROM fixture GROUP BY GROUPING SETS ((k),())', false);
SELECT pg_temp.assert_same('SELECT f.k, count(*), sum(f.a) FROM fixture f JOIN (VALUES (0),(1)) AS v(k) USING(k) GROUP BY f.k', true);

-- An actually empty relation exercises the custom executor's empty-input path.
CREATE TEMP TABLE empty_fixture(k int, a int);
SELECT pg_temp.assert_same('SELECT k, count(*), sum(a) FROM empty_fixture GROUP BY k', true);

-- A materialized scalar subplan with a parameter is rescanned for each outer row.
SET enable_material = off;
SELECT pg_temp.assert_same(
 'SELECT i, (SELECT sum(a) FROM fixture f WHERE f.k = i GROUP BY f.k) AS total FROM generate_series(1,4) i', true);
CREATE INDEX fixture_k_idx ON fixture(k);
SET enable_seqscan = off;
SELECT pg_temp.assert_same(
 'SELECT i, (SELECT sum(a) FROM fixture f WHERE f.k = i GROUP BY f.k) AS total FROM generate_series(1,4) i', true);
RESET enable_seqscan;

-- Cached plan copying, cursor fetch, abort cleanup and recovery in one backend.
SET pg_gftr.enabled = on;
PREPARE grouped(int) AS SELECT k, sum(a), count(*) FROM fixture WHERE a > $1 GROUP BY k;
EXECUTE grouped(4998);
EXECUTE grouped(4999);
DEALLOCATE grouped;
BEGIN;
DECLARE grouped_cursor SCROLL CURSOR FOR SELECT k, min(a) FROM fixture GROUP BY k ORDER BY k;
FETCH 2 FROM grouped_cursor;
FETCH BACKWARD 1 FROM grouped_cursor;
ROLLBACK;

SET pg_gftr.max_input_mb = 1;
CREATE TEMP TABLE too_large AS SELECT i % 8 AS k, i AS a FROM generate_series(1,100000) i;
DO $$
BEGIN
    BEGIN
        PERFORM k, sum(a) FROM too_large GROUP BY k;
        RAISE EXCEPTION 'input budget was not enforced';
    EXCEPTION WHEN program_limit_exceeded THEN NULL;
    END;
END $$;
RESET pg_gftr.max_input_mb;
SELECT pg_temp.assert_same('SELECT k, count(*), sum(a) FROM fixture GROUP BY k', true);

DO $$
BEGIN
    IF position('CUDA not built' IN pg_gftr_version()) > 0 THEN
        PERFORM set_config('pg_gftr.backend', 'cuda', true);
        BEGIN
            PERFORM k, sum(a) FROM fixture GROUP BY k;
            RAISE EXCEPTION 'expected missing CUDA backend error';
        EXCEPTION WHEN OTHERS THEN
            IF SQLERRM NOT LIKE 'pg_gftr: CUDA backend was not built%' THEN RAISE; END IF;
        END;
        PERFORM set_config('pg_gftr.backend', 'cpu', true);
    END IF;
END $$;

EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT k, count(*), sum(a), min(b), max(b) FROM fixture GROUP BY k;
SELECT 'integration tests passed' AS result;
