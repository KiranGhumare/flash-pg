/* PostgreSQL 18 custom upper path for single-int4-key aggregation. */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "portability/instr_time.h"

#include "gftr_backend.h"

#if PG_VERSION_NUM < 180000 || PG_VERSION_NUM >= 190000
#error "pg_gftr currently supports PostgreSQL 18 only"
#endif

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(pg_gftr_version);
void _PG_init(void);
void _PG_fini(void);

enum GftrOp
{
	GFTR_COUNT_STAR = 1,
	GFTR_COUNT,
	GFTR_SUM,
	GFTR_MIN,
	GFTR_MAX
};

typedef struct GftrState
{
	CustomScanState css;
	PlanState *child;
	MemoryContext context;
	MemoryContextCallback cleanup;
	int key_attno;
	int ncols;
	int *attnos;
	List *ops;
	GftrInput input;
	int capacity;
	int max_rows;
	GftrResult result;
	int next_group;
	bool loaded;
	int backend;
	int mode;
	int device;
	size_t device_bytes;
	double pack_ms;
} GftrState;

static bool gftr_enabled = false;
static bool gftr_force = false;
static int gftr_backend = 0;
static int gftr_mode = 0;
static int gftr_device = 0;
static int gftr_min_rows = 100000;
static int gftr_max_input_mb = 256;
static int gftr_max_device_mb = 4096;
static double gftr_setup_cost = 1000.0;
static double gftr_tuple_cost = 0.02;
static create_upper_paths_hook_type previous_upper_hook = NULL;

static const struct config_enum_entry backend_options[] = {
	{"cuda", 0, false}, {"cpu", 1, false}, {NULL, 0, false}
};
static const struct config_enum_entry mode_options[] = {
	{"gftr", 0, false}, {"gfur", 1, false}, {NULL, 0, false}
};

static void gftr_upper_paths(PlannerInfo *, UpperRelationKind,
							 RelOptInfo *, RelOptInfo *, void *);
static Plan *gftr_plan_path(PlannerInfo *, RelOptInfo *, CustomPath *,
							List *, List *, List *);
static Node *gftr_create_state(CustomScan *);
static void gftr_begin(CustomScanState *, EState *, int);
static TupleTableSlot *gftr_exec(CustomScanState *);
static void gftr_end(CustomScanState *);
static void gftr_rescan(CustomScanState *);
static void gftr_explain(CustomScanState *, List *, ExplainState *);

static const CustomPathMethods path_methods = {
	.CustomName = "GftrAggregate",
	.PlanCustomPath = gftr_plan_path
};
static const CustomScanMethods scan_methods = {
	.CustomName = "GftrAggregate",
	.CreateCustomScanState = gftr_create_state
};
static const CustomExecMethods exec_methods = {
	.CustomName = "GftrAggregate",
	.BeginCustomScan = gftr_begin,
	.ExecCustomScan = gftr_exec,
	.EndCustomScan = gftr_end,
	.ReScanCustomScan = gftr_rescan,
	.ExplainCustomScan = gftr_explain
};

Datum
pg_gftr_version(PG_FUNCTION_ARGS)
{
#ifdef GFTR_WITH_CUDA
	PG_RETURN_TEXT_P(cstring_to_text("pg_gftr 0.1.0 (CUDA + CPU reference)"));
#else
	PG_RETURN_TEXT_P(cstring_to_text("pg_gftr 0.1.0 (CPU reference; CUDA not built)"));
#endif
}

void
_PG_init(void)
{
	DefineCustomBoolVariable("pg_gftr.enabled", "Offer GPU aggregate paths.",
		NULL, &gftr_enabled, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("pg_gftr.force", "Prefer supported custom paths for experiments.",
		NULL, &gftr_force, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomEnumVariable("pg_gftr.backend", "Execution backend; CPU is a correctness reference.",
		NULL, &gftr_backend, 0, backend_options, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomEnumVariable("pg_gftr.mode", "GFTR transforms payloads; GFUR gathers by tuple ID.",
		NULL, &gftr_mode, 0, mode_options, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("pg_gftr.device", "CUDA device ordinal in this process.",
		NULL, &gftr_device, 0, 0, 1024, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("pg_gftr.min_rows", "Minimum estimated input rows unless force is on.",
		NULL, &gftr_min_rows, 100000, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("pg_gftr.max_input_mb", "Limit for packed host input in MB.",
		NULL, &gftr_max_input_mb, 256, 1, 16384, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("pg_gftr.max_device_mb", "Limit for this operator's device allocations in MB.",
		NULL, &gftr_max_device_mb, 4096, 1, 1048576, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("pg_gftr.setup_cost", "Estimated GPU setup cost in planner units.",
		NULL, &gftr_setup_cost, 1000.0, 0.0, 1e12, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("pg_gftr.tuple_cost", "Estimated packing, transfer and compute cost per row.",
		NULL, &gftr_tuple_cost, 0.02, 0.0, 1e6, PGC_USERSET, 0, NULL, NULL, NULL);
	MarkGUCPrefixReserved("pg_gftr");
	RegisterCustomScanMethods(&scan_methods);
	previous_upper_hook = create_upper_paths_hook;
	create_upper_paths_hook = gftr_upper_paths;
}

void
_PG_fini(void)
{
	if (create_upper_paths_hook == gftr_upper_paths)
		create_upper_paths_hook = previous_upper_hook;
}

/* Match built-in aggregate OIDs, never names that a search_path can shadow. */
static int
aggregate_op(Aggref *agg)
{
	TargetEntry *arg;
	if (agg->agglevelsup != 0 || agg->aggsplit != AGGSPLIT_SIMPLE ||
		agg->aggkind != AGGKIND_NORMAL || agg->aggdistinct || agg->aggorder ||
		agg->aggfilter || agg->aggdirectargs || agg->aggvariadic)
		return 0;
	if (agg->aggfnoid == F_COUNT_ && agg->aggstar && agg->args == NIL)
		return GFTR_COUNT_STAR;
	if (list_length(agg->args) != 1)
		return 0;
	arg = linitial_node(TargetEntry, agg->args);
	if (arg->resjunk || !IsA(arg->expr, Var) ||
		exprType((Node *) arg->expr) != INT4OID ||
		((Var *) arg->expr)->varlevelsup != 0)
		return 0;
	switch (agg->aggfnoid)
	{
		case F_COUNT_ANY: return GFTR_COUNT;
		case F_SUM_INT4: return GFTR_SUM;
		case F_MIN_INT4: return GFTR_MIN;
		case F_MAX_INT4: return GFTR_MAX;
		default: return 0;
	}
}

static void
gftr_upper_paths(PlannerInfo *root, UpperRelationKind stage,
				 RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	GroupPathExtraData *group_extra = extra;
	SortGroupClause *clause;
	Node *key;
	Path *child;
	CustomPath *path;
	List *scan_tlist = NIL;
	ListCell *lc;
	bool has_aggregate = false;
	int resno = 1;

	if (previous_upper_hook)
		previous_upper_hook(root, stage, input_rel, output_rel, extra);
	if (!gftr_enabled || stage != UPPERREL_GROUP_AGG ||
		!root->parse->hasAggs || list_length(root->parse->groupClause) != 1 ||
		root->parse->groupingSets || root->parse->hasTargetSRFs ||
		root->parse->hasWindowFuncs || group_extra->havingQual ||
		group_extra->patype != PARTITIONWISE_AGGREGATE_NONE)
		return;
	clause = linitial_node(SortGroupClause, root->parse->groupClause);
	key = get_sortgroupclause_expr(clause, root->parse->targetList);
	if (!IsA(key, Var) || exprType(key) != INT4OID ||
		((Var *) key)->varlevelsup != 0 || clause->eqop != Int4EqualOperator)
		return;
	child = input_rel->cheapest_total_path;
	if (!child || child->param_info ||
		(!gftr_force && child->rows < gftr_min_rows) ||
		!list_member(child->pathtarget->exprs, key))
		return;
	scan_tlist = lappend(scan_tlist,
		makeTargetEntry((Expr *) copyObject(key), resno++, NULL, false));
	foreach(lc, output_rel->reltarget->exprs)
	{
		Node *expr = lfirst(lc);
		Aggref *agg;
		if (equal(expr, key))
			continue;
		if (!IsA(expr, Aggref) || !aggregate_op((Aggref *) expr))
			return;
		agg = (Aggref *) expr;
		if (agg->args != NIL)
		{
			TargetEntry *arg = linitial_node(TargetEntry, agg->args);
			if (!list_member(child->pathtarget->exprs, arg->expr))
				return;
		}
		if (!tlist_member((Expr *) expr, scan_tlist))
			scan_tlist = lappend(scan_tlist,
				makeTargetEntry((Expr *) copyObject(expr), resno++, NULL, false));
		has_aggregate = true;
	}
	if (!has_aggregate)
		return;
	path = makeNode(CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parent = output_rel;
	path->path.pathtarget = output_rel->reltarget;
	path->path.param_info = NULL;
	path->path.parallel_aware = false;
	path->path.parallel_safe = false;
	path->path.parallel_workers = 0;
	path->path.pathkeys = NIL;
	/* Upper relations do not necessarily set RelOptInfo.rows. */
	path->path.rows = ((Path *) linitial(output_rel->pathlist))->rows;
	path->path.startup_cost = child->total_cost + gftr_setup_cost +
		child->rows * gftr_tuple_cost;
	path->path.total_cost = path->path.startup_cost +
		path->path.rows * cpu_tuple_cost;
	/* Explicit experimental override; no claim of calibrated GPU costs. */
	if (gftr_force)
	{
		path->path.startup_cost = 0;
		path->path.total_cost = 0.001;
	}
	path->custom_paths = list_make1(child);
	path->custom_private = scan_tlist;
	path->methods = &path_methods;
	add_path(output_rel, &path->path);
}

static Plan *
gftr_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *path,
			   List *tlist, List *clauses, List *children)
{
	CustomScan *scan = makeNode(CustomScan);
	Plan *child = (Plan *) linitial(children);
	List *scan_tlist = path->custom_private;
	TargetEntry *key_entry;
	List *attrs = NIL;
	List *ops = NIL;
	ListCell *lc;

	key_entry = tlist_member(linitial_node(TargetEntry, scan_tlist)->expr,
							 child->targetlist);
	if (!key_entry)
		elog(ERROR, "pg_gftr: grouping key missing from child target list");
	for_each_from(lc, scan_tlist, 1)
	{
		Aggref *agg = (Aggref *) lfirst_node(TargetEntry, lc)->expr;
		int op = aggregate_op(agg);
		int col = -1;
		if (op != GFTR_COUNT_STAR)
		{
			TargetEntry *arg = linitial_node(TargetEntry, agg->args);
			TargetEntry *entry = tlist_member(arg->expr, child->targetlist);
			ListCell *ac;
			if (!entry)
				elog(ERROR, "pg_gftr: aggregate input missing from child target list");
			col = 0;
			foreach(ac, attrs)
			{
				if (lfirst_int(ac) == entry->resno)
					break;
				col++;
			}
			if (col == list_length(attrs))
				attrs = lappend_int(attrs, entry->resno);
		}
		ops = lappend(ops, list_make2_int(op, col));
	}
	scan->scan.scanrelid = 0;
	scan->scan.plan.targetlist = tlist;
	scan->custom_scan_tlist = copyObject(scan_tlist);
	scan->custom_plans = children;
	scan->custom_private = list_make3(makeInteger(key_entry->resno), attrs, ops);
	scan->methods = &scan_methods;
	return &scan->scan.plan;
}

static Node *
gftr_create_state(CustomScan *scan)
{
	GftrState *state = palloc0(sizeof(GftrState));
	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &exec_methods;
	return (Node *) state;
}

static void
gftr_cleanup(void *arg)
{
	GftrState *state = arg;
	gftr_result_free(&state->result);
}

static void
gftr_begin(CustomScanState *node, EState *estate, int eflags)
{
	GftrState *state = (GftrState *) node;
	CustomScan *plan = (CustomScan *) node->ss.ps.plan;
	List *attrs = lsecond(plan->custom_private);
	ListCell *lc;
	uint64 row_bytes;
	uint64 max_rows;
	int c = 0;

	state->context = estate->es_query_cxt;
	state->key_attno = intVal(linitial(plan->custom_private));
	state->ncols = list_length(attrs);
	state->attnos = palloc(sizeof(int) * Max(1, state->ncols));
	foreach(lc, attrs)
		state->attnos[c++] = lfirst_int(lc);
	state->ops = lthird(plan->custom_private);
	state->input.ncols = state->ncols;
	state->input.columns = palloc0(sizeof(GftrValue *) * Max(1, state->ncols));
	state->backend = gftr_backend;
	state->mode = gftr_mode;
	state->device = gftr_device;
	state->device_bytes = (size_t) gftr_max_device_mb * 1024 * 1024;
	row_bytes = sizeof(uint64_t) + (uint64) state->ncols * sizeof(GftrValue);
	max_rows = (uint64) gftr_max_input_mb * 1024 * 1024 / row_bytes;
	/* Every palloc and CUB's item count must remain representable. */
	max_rows = Min(max_rows, (uint64) MaxAllocSize / sizeof(uint64_t));
	state->max_rows = (int) Min(max_rows, (uint64) INT_MAX);
	state->cleanup.func = gftr_cleanup;
	state->cleanup.arg = state;
	MemoryContextRegisterResetCallback(state->context, &state->cleanup);
	state->child = ExecInitNode((Plan *) linitial(plan->custom_plans), estate, eflags);
	node->custom_ps = list_make1(state->child);
}

static void
grow_input(GftrState *state)
{
	int capacity;
	int c;
	if (state->input.nrows >= state->max_rows)
		ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("pg_gftr packed input limit exceeded"),
			 errhint("Increase pg_gftr.max_input_mb or disable pg_gftr for this query.")));
	capacity = (int) Min((int64) state->max_rows,
						state->capacity ? (int64) state->capacity * 2 : 8192);
	if (!state->input.keys)
		state->input.keys = palloc((Size) capacity * sizeof(uint64_t));
	else
		state->input.keys = repalloc((void *) state->input.keys,
									(Size) capacity * sizeof(uint64_t));
	for (c = 0; c < state->ncols; c++)
	{
		if (!state->input.columns[c])
			state->input.columns[c] = palloc((Size) capacity * sizeof(GftrValue));
		else
			state->input.columns[c] = repalloc(state->input.columns[c],
											  (Size) capacity * sizeof(GftrValue));
	}
	state->capacity = capacity;
}

static void
load_input(GftrState *state)
{
	MemoryContext old = MemoryContextSwitchTo(state->context);
	TupleTableSlot *slot;
	instr_time start;
	instr_time end;
	char error[GFTR_ERROR_SIZE] = {0};
	int status;

	INSTR_TIME_SET_CURRENT(start);
	for (;;)
	{
		int row = state->input.nrows;
		bool isnull;
		Datum key;
		int c;
		/* TupIsNull is a macro: never pass an expression with side effects. */
		slot = ExecProcNode(state->child);
		if (TupIsNull(slot))
			break;
		CHECK_FOR_INTERRUPTS();
		if (row == state->capacity)
			grow_input(state);
		key = slot_getattr(slot, state->key_attno, &isnull);
		((uint64_t *) state->input.keys)[row] = isnull ? GFTR_NULL_KEY :
			(uint64_t) ((int64) DatumGetInt32(key) - PG_INT32_MIN);
		for (c = 0; c < state->ncols; c++)
		{
			Datum v = slot_getattr(slot, state->attnos[c], &isnull);
			state->input.columns[c][row].value = isnull ? 0 : DatumGetInt32(v);
			state->input.columns[c][row].valid = !isnull;
		}
		state->input.nrows++;
	}
	INSTR_TIME_SET_CURRENT(end);
	INSTR_TIME_SUBTRACT(end, start);
	state->pack_ms = INSTR_TIME_GET_MILLISEC(end);
	if (state->backend == 1)
		status = gftr_cpu_compute(&state->input, &state->result, error);
	else
		status = gftr_cuda_compute(&state->input, state->mode == 0,
			state->device, state->device_bytes, &state->result, error);
	CHECK_FOR_INTERRUPTS();
	if (status)
		ereport(ERROR, (errmsg("pg_gftr: %s", error)));
	state->loaded = true;
	MemoryContextSwitchTo(old);
}

static TupleTableSlot *
gftr_access(ScanState *scan)
{
	GftrState *state = (GftrState *) scan;
	TupleTableSlot *slot = scan->ss_ScanTupleSlot;
	int group;
	int out = 1;
	ListCell *lc;
	if (!state->loaded)
		load_input(state);
	ExecClearTuple(slot);
	if (state->next_group >= state->result.ngroups)
		return slot;
	group = state->next_group++;
	memset(slot->tts_isnull, 0, sizeof(bool) * slot->tts_tupleDescriptor->natts);
	if (state->result.keys[group] == GFTR_NULL_KEY)
		slot->tts_isnull[0] = true;
	else
		slot->tts_values[0] = Int32GetDatum((int32)
			((int64) state->result.keys[group] + PG_INT32_MIN));
	foreach(lc, state->ops)
	{
		List *op = lfirst(lc);
		int kind = linitial_int(op);
		int col = lsecond_int(op);
		GftrColumnStats *s;
		if (kind == GFTR_COUNT_STAR)
		{
			slot->tts_values[out++] = Int64GetDatum(state->result.counts[group]);
			continue;
		}
		s = &state->result.columns[col][group];
		if (kind == GFTR_COUNT)
			slot->tts_values[out] = Int64GetDatum(s->count);
		else if (s->count == 0)
			slot->tts_isnull[out] = true;
		else if (kind == GFTR_SUM)
			slot->tts_values[out] = Int64GetDatum(s->sum);
		else if (kind == GFTR_MIN)
			slot->tts_values[out] = Int32GetDatum(s->min);
		else
			slot->tts_values[out] = Int32GetDatum(s->max);
		out++;
	}
	return ExecStoreVirtualTuple(slot);
}

static bool
gftr_recheck(ScanState *state, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
gftr_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, gftr_access, gftr_recheck);
}

static void
gftr_end(CustomScanState *node)
{
	GftrState *state = (GftrState *) node;
	ExecEndNode(state->child);
	gftr_result_free(&state->result);
}

static void
gftr_rescan(CustomScanState *node)
{
	GftrState *state = (GftrState *) node;
	gftr_result_free(&state->result);
	state->loaded = false;
	state->input.nrows = 0;
	state->next_group = 0;
	state->pack_ms = 0;
	/* custom_ps children are not outerPlan/innerPlan; propagate params here. */
	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(state->child, node->ss.ps.chgParam);
	if (state->child->chgParam == NULL)
		ExecReScan(state->child);
}

static void
gftr_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GftrState *state = (GftrState *) node;
	ExplainPropertyText("Backend", state->backend ? "CPU reference" : "CUDA", es);
	ExplainPropertyText("Algorithm", state->backend ? "CPU sort reference" :
		(state->mode ? "GFUR sort aggregation" : "GFTR sort aggregation"), es);
	ExplainPropertyInteger("Payload Columns", NULL, state->ncols, es);
	if (es->analyze && state->loaded)
	{
		ExplainPropertyText("Device", state->result.device_name, es);
		ExplainPropertyInteger("Input Rows", NULL, state->input.nrows, es);
		ExplainPropertyInteger("Groups", NULL, state->result.ngroups, es);
		ExplainPropertyFloat("Scan and Pack", "ms", state->pack_ms, 3, es);
		ExplainPropertyFloat("Backend Total", "ms", state->result.total_ms, 3, es);
		ExplainPropertyFloat("Sort", "ms", state->result.sort_ms, 3, es);
		ExplainPropertyFloat("Group and Reduce", "ms", state->result.reduce_ms, 3, es);
		ExplainPropertyFloat("Transfer", "ms", state->result.transfer_ms, 3, es);
	}
}
