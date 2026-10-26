\echo Use "CREATE EXTENSION pg_gftr" to load this file. \quit

CREATE FUNCTION pg_gftr_version() RETURNS text
AS 'MODULE_PATHNAME', 'pg_gftr_version'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

-- Loading the C function registers the planner hook in the creating session.
SELECT pg_gftr_version();
