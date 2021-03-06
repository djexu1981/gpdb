/*-------------------------------------------------------------------------
 *
 * createplan.c
 *	  Routines to create the desired plan for processing a query.
 *	  Planning is complete, we just need to convert the selected
 *	  Path into a Plan.
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/createplan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "catalog/pg_exttable.h"
#include "access/stratnum.h"
#include "access/sysattr.h"
#include "catalog/pg_class.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "executor/executor.h"
#include "executor/execHHashagg.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/planpartition.h"
#include "optimizer/planshare.h"
#include "optimizer/predtest.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "parser/parse_oper.h"	/* ordering_oper_opid */
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/uri.h"

#include "cdb/cdbhash.h"
#include "cdb/cdbllize.h"		/* cdbllize_adjust_init_plan_path() */
#include "cdb/cdbmutate.h"
#include "cdb/cdbpartition.h"
#include "cdb/cdbpath.h"		/* cdbpath_rows() */
#include "cdb/cdbpathtoplan.h"	/* cdbpathtoplan_create_flow() etc. */
#include "cdb/cdbpullup.h"		/* cdbpullup_targetlist() */
#include "cdb/cdbsetop.h"
#include "cdb/cdbsreh.h"
#include "cdb/cdbtargeteddispatch.h"
#include "cdb/cdbvars.h"

/*
 * Flag bits that can appear in the flags argument of create_plan_recurse().
 * These can be OR-ed together.
 *
 * CP_EXACT_TLIST specifies that the generated plan node must return exactly
 * the tlist specified by the path's pathtarget (this overrides both
 * CP_SMALL_TLIST and CP_LABEL_TLIST, if those are set).  Otherwise, the
 * plan node is allowed to return just the Vars and PlaceHolderVars needed
 * to evaluate the pathtarget.
 *
 * CP_SMALL_TLIST specifies that a narrower tlist is preferred.  This is
 * passed down by parent nodes such as Sort and Hash, which will have to
 * store the returned tuples.
 *
 * CP_LABEL_TLIST specifies that the plan node must return columns matching
 * any sortgrouprefs specified in its pathtarget, with appropriate
 * ressortgroupref labels.  This is passed down by parent nodes such as Sort
 * and Group, which need these values to be available in their inputs.
 */
#define CP_EXACT_TLIST		0x0001		/* Plan must return specified tlist */
#define CP_SMALL_TLIST		0x0002		/* Prefer narrower tlists */
#define CP_LABEL_TLIST		0x0004		/* tlist must contain sortgrouprefs */


static Plan *create_scan_plan(PlannerInfo *root, Path *best_path,
				 int flags);
static List *build_path_tlist(PlannerInfo *root, Path *path);
static bool use_physical_tlist(PlannerInfo *root, Path *path, int flags);
static List *get_gating_quals(PlannerInfo *root, List *quals);
static Plan *create_gating_plan(PlannerInfo *root, Path *path, Plan *plan,
				   List *gating_quals);
static Plan *create_join_plan(PlannerInfo *root, JoinPath *best_path);
static Plan *create_append_plan(PlannerInfo *root, AppendPath *best_path);
static Plan *create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path);
static Result *create_result_plan(PlannerInfo *root, ResultPath *best_path);
static Material *create_material_plan(PlannerInfo *root, MaterialPath *best_path,
					 int flags);
static Plan *create_unique_plan(PlannerInfo *root, UniquePath *best_path,
				   int flags);
static Plan *create_motion_plan(PlannerInfo *root, CdbMotionPath *path);
static Plan *create_splitupdate_plan(PlannerInfo *root, SplitUpdatePath *path);
static Gather *create_gather_plan(PlannerInfo *root, GatherPath *best_path);
static Plan *create_projection_plan(PlannerInfo *root, ProjectionPath *best_path);
static Plan *inject_projection_plan(Plan *subplan, List *tlist);
static Sort *create_sort_plan(PlannerInfo *root, SortPath *best_path, int flags);
static Unique *create_upper_unique_plan(PlannerInfo *root, UpperUniquePath *best_path,
						 int flags);
static Agg *create_agg_plan(PlannerInfo *root, AggPath *best_path);
static Plan *create_groupingsets_plan(PlannerInfo *root, GroupingSetsPath *best_path);
static Result *create_minmaxagg_plan(PlannerInfo *root, MinMaxAggPath *best_path);
static WindowAgg *create_windowagg_plan(PlannerInfo *root, WindowAggPath *best_path);
static SetOp *create_setop_plan(PlannerInfo *root, SetOpPath *best_path,
				  int flags);
static RecursiveUnion *create_recursiveunion_plan(PlannerInfo *root, RecursiveUnionPath *best_path);
static void get_column_info_for_window(PlannerInfo *root, WindowClause *wc,
						   List *tlist,
						   int numSortCols, AttrNumber *sortColIdx,
						   int *partNumCols,
						   AttrNumber **partColIdx,
						   Oid **partOperators,
						   int *ordNumCols,
						   AttrNumber **ordColIdx,
						   Oid **ordOperators);
static LockRows *create_lockrows_plan(PlannerInfo *root, LockRowsPath *best_path,
					 int flags);
static ModifyTable *create_modifytable_plan(PlannerInfo *root, ModifyTablePath *best_path);
static Limit *create_limit_plan(PlannerInfo *root, LimitPath *best_path,
				  int flags);
static SeqScan *create_seqscan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses);
static ExternalScan *create_externalscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses);
static SampleScan *create_samplescan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses);
static Scan *create_indexscan_plan(PlannerInfo *root, IndexPath *best_path,
					  List *tlist, List *scan_clauses, bool indexonly);
static BitmapHeapScan *create_bitmap_scan_plan(PlannerInfo *root,
						BitmapHeapPath *best_path,
						List *tlist, List *scan_clauses);
static Plan *create_bitmap_subplan(PlannerInfo *root, Path *bitmapqual,
					  List **qual, List **indexqual, List **indexECs);
static TidScan *create_tidscan_plan(PlannerInfo *root, TidPath *best_path,
					List *tlist, List *scan_clauses);
static SubqueryScan *create_subqueryscan_plan(PlannerInfo *root,
						 SubqueryScanPath *best_path,
						 List *tlist, List *scan_clauses);
static FunctionScan *create_functionscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses);
static TableFunctionScan *create_tablefunction_plan(PlannerInfo *root,
						  TableFunctionScanPath *best_path,
						  List *tlist,
						  List *scan_clauses);
static ValuesScan *create_valuesscan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses);
static Plan *create_ctescan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses);
static WorkTableScan *create_worktablescan_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses);
static ForeignScan *create_foreignscan_plan(PlannerInfo *root, ForeignPath *best_path,
						List *tlist, List *scan_clauses);
static CustomScan *create_customscan_plan(PlannerInfo *root,
					   CustomPath *best_path,
					   List *tlist, List *scan_clauses);
static NestLoop *create_nestloop_plan(PlannerInfo *root, NestPath *best_path);
static MergeJoin *create_mergejoin_plan(PlannerInfo *root, MergePath *best_path);
static HashJoin *create_hashjoin_plan(PlannerInfo *root, HashPath *best_path);
static Node *replace_nestloop_params(PlannerInfo *root, Node *expr);
static Node *replace_nestloop_params_mutator(Node *node, PlannerInfo *root);
static void process_subquery_nestloop_params(PlannerInfo *root,
								 List *subplan_params);
static List *fix_indexqual_references(PlannerInfo *root, IndexPath *index_path);
static List *fix_indexorderby_references(PlannerInfo *root, IndexPath *index_path);
static Node *fix_indexqual_operand(Node *node, IndexOptInfo *index, int indexcol);
static List *get_switched_clauses(List *clauses, Relids outerrelids);
static List *order_qual_clauses(PlannerInfo *root, List *clauses);
static void copy_generic_path_info(Plan *dest, Path *src);
static void copy_plan_costsize(Plan *dest, Plan *src);
static void label_sort_with_costsize(PlannerInfo *root, Sort *plan,
						 double limit_tuples);
static SeqScan *make_seqscan(List *qptlist, List *qpqual, Index scanrelid);
static ExternalScan *make_externalscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  List *filenames,
				  char *fmtoptstring,
				  bool istext,
				  bool ismasteronly,
				  int rejectlimit,
				  bool rejectlimitinrows,
				  bool logerrors,
				  int encoding);
static SampleScan *make_samplescan(List *qptlist, List *qpqual, Index scanrelid,
				TableSampleClause *tsc);
static IndexScan *make_indexscan(List *qptlist, List *qpqual, Index scanrelid,
			   Oid indexid, List *indexqual, List *indexqualorig,
			   List *indexorderby, List *indexorderbyorig,
			   List *indexorderbyops,
			   ScanDirection indexscandir);
static IndexOnlyScan *make_indexonlyscan(List *qptlist, List *qpqual,
				   Index scanrelid, Oid indexid,
				   List *indexqual, List *indexqualorig,
				   List *indexorderby,
				   List *indextlist,
				   ScanDirection indexscandir);
static BitmapIndexScan *make_bitmap_indexscan(Index scanrelid, Oid indexid,
					  List *indexqual,
					  List *indexqualorig);
static BitmapHeapScan *make_bitmap_heapscan(List *qptlist,
					 List *qpqual,
					 Plan *lefttree,
					 List *bitmapqualorig,
					 Index scanrelid);
static TidScan *make_tidscan(List *qptlist, List *qpqual, Index scanrelid,
			 List *tidquals);
static FunctionScan *make_functionscan(List *qptlist, List *qpqual,
				  Index scanrelid, List *functions, bool funcordinality);
static TableFunctionScan *make_tablefunction(List *qptlist, List *qpqual,
				   Plan *subplan, Index scanrelid, RangeTblFunction *function);
static ValuesScan *make_valuesscan(List *qptlist, List *qpqual,
				Index scanrelid, List *values_lists);
static CteScan *make_ctescan(List *qptlist, List *qpqual,
			 Index scanrelid, int ctePlanId, int cteParam);
static WorkTableScan *make_worktablescan(List *qptlist, List *qpqual,
				   Index scanrelid, int wtParam);
static Append *make_append(List *appendplans, List *tlist);
static RecursiveUnion *make_recursive_union(List *tlist,
					 Plan *lefttree,
					 Plan *righttree,
					 int wtParam,
					 List *distinctList,
					 long numGroups);
static BitmapAnd *make_bitmap_and(List *bitmapplans);
static BitmapOr *make_bitmap_or(List *bitmapplans);
static Sort *make_sort(Plan *lefttree, int numCols,
		  AttrNumber *sortColIdx, Oid *sortOperators,
		  Oid *collations, bool *nullsFirst);
static Plan *prepare_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
						   Relids relids,
						   const AttrNumber *reqColIdx,
						   bool adjust_tlist_in_place,
						   int *p_numsortkeys,
						   AttrNumber **p_sortColIdx,
						   Oid **p_sortOperators,
						   Oid **p_collations,
						   bool **p_nullsFirst, bool add_keys_to_targetlist);
static EquivalenceMember *find_ec_member_for_tle(EquivalenceClass *ec,
					   TargetEntry *tle,
					   Relids relids);
static WindowAgg *make_windowagg(List *tlist, Index winref,
			   int partNumCols, AttrNumber *partColIdx, Oid *partOperators,
			   int ordNumCols, AttrNumber *ordColIdx, Oid *ordOperators,
			   AttrNumber firstOrderCol, Oid firstOrderCmpOperator, bool firstOrderNullsFirst,
			   int frameOptions, Node *startOffset, Node *endOffset,
			   Plan *lefttree);
static Unique *make_unique_from_sortclauses(Plan *lefttree, List *distinctList);
static Unique *make_unique_from_pathkeys(Plan *lefttree,
						  List *pathkeys, int numCols);
static Gather *make_gather(List *qptlist, List *qpqual,
			int nworkers, bool single_copy, Plan *subplan);
static SetOp *make_setop(SetOpCmd cmd, SetOpStrategy strategy, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx, int firstFlag,
		   long numGroups);
static LockRows *make_lockrows(Plan *lefttree, List *rowMarks, int epqParam);
static ModifyTable *make_modifytable(PlannerInfo *root,
				 CmdType operation, bool canSetTag,
				 Index nominalRelation,
				 List *resultRelations, List *subplans,
				 List *withCheckOptionLists, List *returningLists,
				 List *is_split_updates,
				 List *rowMarks, OnConflictExpr *onconflict, int epqParam);

static TargetEntry *find_junk_tle(List *targetList, const char *junkAttrName);
static Motion *cdbpathtoplan_create_motion_plan(PlannerInfo *root,
								 CdbMotionPath *path,
								 Plan *subplan);

/*
 * create_plan
 *	  Creates the access plan for a query by recursively processing the
 *	  desired tree of pathnodes, starting at the node 'best_path'.  For
 *	  every pathnode found, we create a corresponding plan node containing
 *	  appropriate id, target list, and qualification information.
 *
 *	  The tlists and quals in the plan tree are still in planner format,
 *	  ie, Vars still correspond to the parser's numbering.  This will be
 *	  fixed later by setrefs.c.
 *
 *	  best_path is the best access path
 *
 *	  Returns a Plan tree.
 */
Plan *
create_plan(PlannerInfo *root, Path *best_path, PlanSlice *curSlice)
{
	Plan	   *plan;

	root->curSlice = curSlice;

	/* plan_params should not be in use in current query level */
	Assert(root->plan_params == NIL);

	/* Modify path to support unique rowid operation for subquery preds. */
	if (root->join_info_list)
		cdbpath_dedup_fixup(root, best_path);

	/* Initialize this module's workspace in PlannerInfo */
	root->curOuterRels = NULL;
	root->curOuterParams = NIL;

	/* Recursively process the path tree, demanding the correct tlist result */
	plan = create_plan_recurse(root, best_path, CP_EXACT_TLIST);

	/*
	 * Make sure the topmost plan node's targetlist exposes the original
	 * column names and other decorative info.  Targetlists generated within
	 * the planner don't bother with that stuff, but we must have it on the
	 * top-level tlist seen at execution time.  However, ModifyTable plan
	 * nodes don't have a tlist matching the querytree targetlist.
	 *
	 * The ModifyTable might be under a Motion, so peek underneath it.
	 */
	{
		Plan	   *topplan = plan;

		if (IsA(plan, Motion))
			topplan = plan->lefttree;

		if (!IsA(topplan, ModifyTable))
			apply_tlist_labeling(topplan->targetlist, root->processed_tlist);
	}

	/* Decorate the top node of the plan with a Flow node. */
	plan->flow = cdbpathtoplan_create_flow(root, best_path->locus);

	/*
	 * Attach any initPlans created in this query level to the topmost plan
	 * node.  (In principle the initplans could go in any plan node at or
	 * above where they're referenced, but there seems no reason to put them
	 * any lower than the topmost node for the query level.  Also, see
	 * comments for SS_finalize_plan before you try to change this.)
	 */
	SS_attach_initplans(root, plan);

	/* Check we successfully assigned all NestLoopParams to plan nodes */
	if (root->curOuterParams != NIL)
		elog(ERROR, "failed to assign all NestLoopParams to plan nodes");

	/*
	 * Reset plan_params to ensure param IDs used for nestloop params are not
	 * re-used later
	 */
	root->plan_params = NIL;

	return plan;
}

/*
 * create_plan_recurse
 *	  Recursive guts of create_plan().
 */
Plan *
create_plan_recurse(PlannerInfo *root, Path *best_path, int flags)
{
	Plan	   *plan;

	/* Guard against stack overflow due to overly complex plans */
	check_stack_depth();

	switch (best_path->pathtype)
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_ExternalScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFunctionScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_ForeignScan:
		case T_CustomScan:
			plan = create_scan_plan(root, best_path, flags);
			break;
		case T_HashJoin:
		case T_MergeJoin:
		case T_NestLoop:
			plan = create_join_plan(root,
									(JoinPath *) best_path);
			break;
		case T_Append:
			plan = create_append_plan(root,
									  (AppendPath *) best_path);
			break;
		case T_MergeAppend:
			plan = create_merge_append_plan(root,
											(MergeAppendPath *) best_path);
			break;
		case T_Result:
			if (IsA(best_path, ProjectionPath))
			{
				plan = create_projection_plan(root,
											  (ProjectionPath *) best_path);
			}
			else if (IsA(best_path, MinMaxAggPath))
			{
				plan = (Plan *) create_minmaxagg_plan(root,
												(MinMaxAggPath *) best_path);
			}
			else
			{
				Assert(IsA(best_path, ResultPath));
				plan = (Plan *) create_result_plan(root,
												   (ResultPath *) best_path);
			}
			break;
		case T_Material:
			plan = (Plan *) create_material_plan(root,
												 (MaterialPath *) best_path,
												 flags);
			break;
		case T_Unique:
			if (IsA(best_path, UpperUniquePath))
			{
				plan = (Plan *) create_upper_unique_plan(root,
											   (UpperUniquePath *) best_path,
														 flags);
			}
			else
			{
				Assert(IsA(best_path, UniquePath));
				plan = create_unique_plan(root,
										  (UniquePath *) best_path,
										  flags);
			}
			break;
		case T_Gather:
			plan = (Plan *) create_gather_plan(root,
											   (GatherPath *) best_path);
			break;
		case T_Sort:
			plan = (Plan *) create_sort_plan(root,
											 (SortPath *) best_path,
											 flags);
			break;
		case T_Agg:
			if (IsA(best_path, GroupingSetsPath))
				plan = create_groupingsets_plan(root,
											 (GroupingSetsPath *) best_path);
			else
			{
				Assert(IsA(best_path, AggPath));
				plan = (Plan *) create_agg_plan(root,
												(AggPath *) best_path);
			}
			break;
		case T_WindowAgg:
			plan = (Plan *) create_windowagg_plan(root,
												(WindowAggPath *) best_path);
			break;
		case T_SetOp:
			plan = (Plan *) create_setop_plan(root,
											  (SetOpPath *) best_path,
											  flags);
			break;
		case T_RecursiveUnion:
			plan = (Plan *) create_recursiveunion_plan(root,
										   (RecursiveUnionPath *) best_path);
			break;
		case T_LockRows:
			plan = (Plan *) create_lockrows_plan(root,
												 (LockRowsPath *) best_path,
												 flags);
			break;
		case T_ModifyTable:
			plan = (Plan *) create_modifytable_plan(root,
											  (ModifyTablePath *) best_path);
			break;
		case T_Limit:
			plan = (Plan *) create_limit_plan(root,
											  (LimitPath *) best_path,
											  flags);
			break;
		case T_Motion:
			plan = create_motion_plan(root, (CdbMotionPath *) best_path);
			break;
		case T_PartitionSelector:
			plan = create_partition_selector_plan(root, (PartitionSelectorPath *) best_path);
			break;
		case T_SplitUpdate:
			plan = create_splitupdate_plan(root, (SplitUpdatePath *) best_path);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

	return plan;
}

/*
 * create_scan_plan
 *	 Create a scan plan for the parent relation of 'best_path'.
 */
static Plan *
create_scan_plan(PlannerInfo *root, Path *best_path, int flags)
{
	RelOptInfo *rel = best_path->parent;
	List	   *scan_clauses;
	List	   *gating_clauses;
	List	   *tlist;
	Plan	   *plan;

	/*
	 * Extract the relevant restriction clauses from the parent relation. The
	 * executor must apply all these restrictions during the scan, except for
	 * pseudoconstants which we'll take care of below.
	 *
	 * If this is a plain indexscan or index-only scan, we need not consider
	 * restriction clauses that are implied by the index's predicate, so use
	 * indrestrictinfo not baserestrictinfo.  Note that we can't do that for
	 * bitmap indexscans, since there's not necessarily a single index
	 * involved; but it doesn't matter since create_bitmap_scan_plan() will be
	 * able to get rid of such clauses anyway via predicate proof.
	 */
	switch (best_path->pathtype)
	{
		case T_IndexScan:
		case T_IndexOnlyScan:
			Assert(IsA(best_path, IndexPath));
			scan_clauses = ((IndexPath *) best_path)->indexinfo->indrestrictinfo;
			break;
		default:
			scan_clauses = rel->baserestrictinfo;
			break;
	}

	/*
	 * If this is a parameterized scan, we also need to enforce all the join
	 * clauses available from the outer relation(s).
	 *
	 * For paranoia's sake, don't modify the stored baserestrictinfo list.
	 */
	if (best_path->param_info)
		scan_clauses = list_concat(list_copy(scan_clauses),
								   best_path->param_info->ppi_clauses);

	/*
	 * Detect whether we have any pseudoconstant quals to deal with.  Then, if
	 * we'll need a gating Result node, it will be able to project, so there
	 * are no requirements on the child's tlist.
	 */
	gating_clauses = get_gating_quals(root, scan_clauses);
	if (gating_clauses)
		flags = 0;

	/*
	 * For table scans, rather than using the relation targetlist (which is
	 * only those Vars actually needed by the query), we prefer to generate a
	 * tlist containing all Vars in order.  This will allow the executor to
	 * optimize away projection of the table tuples, if possible.
	 */
	if (use_physical_tlist(root, best_path, flags))
	{
		if (best_path->pathtype == T_IndexOnlyScan)
		{
			/* For index-only scan, the preferred tlist is the index's */
			tlist = copyObject(((IndexPath *) best_path)->indexinfo->indextlist);

			/*
			 * Transfer any sortgroupref data to the replacement tlist, unless
			 * we don't care because the gating Result will handle it.
			 */
			if (!gating_clauses)
				apply_pathtarget_labeling_to_tlist(tlist, best_path->pathtarget);
		}
		else
		{
			tlist = build_physical_tlist(root, rel);
			if (tlist == NIL)
			{
				/* Failed because of dropped cols, so use regular method */
				tlist = build_path_tlist(root, best_path);
			}
			else
			{
				/* As above, transfer sortgroupref data to replacement tlist */
				if (!gating_clauses)
					apply_pathtarget_labeling_to_tlist(tlist, best_path->pathtarget);
			}
		}
	}
	else
	{
		tlist = build_path_tlist(root, best_path);
	}

	switch (best_path->pathtype)
	{
		case T_SeqScan:
			plan = (Plan *) create_seqscan_plan(root,
												best_path,
												tlist,
												scan_clauses);
			break;

		case T_ExternalScan:
			plan = (Plan *) create_externalscan_plan(root,
													 best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_SampleScan:
			plan = (Plan *) create_samplescan_plan(root,
												   best_path,
												   tlist,
												   scan_clauses);
			break;

		case T_IndexScan:
			plan = (Plan *) create_indexscan_plan(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses,
												  false);
			break;

		case T_IndexOnlyScan:
			plan = (Plan *) create_indexscan_plan(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses,
												  true);
			break;

		case T_BitmapHeapScan:
			plan = (Plan *) create_bitmap_scan_plan(root,
												(BitmapHeapPath *) best_path,
													tlist,
													scan_clauses);
			break;

		case T_TidScan:
			plan = (Plan *) create_tidscan_plan(root,
												(TidPath *) best_path,
												tlist,
												scan_clauses);
			break;

		case T_SubqueryScan:
			plan = (Plan *) create_subqueryscan_plan(root,
											  (SubqueryScanPath *) best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_FunctionScan:
			plan = (Plan *) create_functionscan_plan(root,
													 best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_TableFunctionScan:
			plan = (Plan *) create_tablefunction_plan(root,
													  (TableFunctionScanPath *) best_path,
													  tlist,
													  scan_clauses);
			break;

		case T_ValuesScan:
			plan = (Plan *) create_valuesscan_plan(root,
												   best_path,
												   tlist,
												   scan_clauses);
			break;

		case T_CteScan:
			plan = (Plan *) create_ctescan_plan(root,
												best_path,
												tlist,
												scan_clauses);
			break;

		case T_WorkTableScan:
			plan = (Plan *) create_worktablescan_plan(root,
													  best_path,
													  tlist,
													  scan_clauses);
			break;

		case T_ForeignScan:
			plan = (Plan *) create_foreignscan_plan(root,
													(ForeignPath *) best_path,
													tlist,
													scan_clauses);
			break;

		case T_CustomScan:
			plan = (Plan *) create_customscan_plan(root,
												   (CustomPath *) best_path,
												   tlist,
												   scan_clauses);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

	if (Gp_role == GP_ROLE_DISPATCH && root->config->gp_enable_direct_dispatch)
		DirectDispatchUpdateContentIdsFromPlan(root, plan);

	/*
	 * If there are any pseudoconstant clauses attached to this node, insert a
	 * gating Result node that evaluates the pseudoconstants as one-time
	 * quals.
	 */
	if (gating_clauses)
		plan = create_gating_plan(root, best_path, plan, gating_clauses);

	return plan;
}

/*
 * Build a target list (ie, a list of TargetEntry) for the Path's output.
 *
 * This is almost just make_tlist_from_pathtarget(), but we also have to
 * deal with replacing nestloop params.
 */
static List *
build_path_tlist(PlannerInfo *root, Path *path)
{
	List	   *tlist = NIL;
	Index	   *sortgrouprefs = path->pathtarget->sortgrouprefs;
	int			resno = 1;
	ListCell   *v;

	foreach(v, path->pathtarget->exprs)
	{
		Node	   *node = (Node *) lfirst(v);
		TargetEntry *tle;

		/*
		 * If it's a parameterized path, there might be lateral references in
		 * the tlist, which need to be replaced with Params.  There's no need
		 * to remake the TargetEntry nodes, so apply this to each list item
		 * separately.
		 */
		if (path->param_info)
			node = replace_nestloop_params(root, node);

		tle = makeTargetEntry((Expr *) node,
							  resno,
							  NULL,
							  false);
		if (sortgrouprefs)
			tle->ressortgroupref = sortgrouprefs[resno - 1];

		tlist = lappend(tlist, tle);
		resno++;
	}
	return tlist;
}

/*
 * use_physical_tlist
 *		Decide whether to use a tlist matching relation structure,
 *		rather than only those Vars actually referenced.
 */
static bool
use_physical_tlist(PlannerInfo *root, Path *path, int flags)
{
	RelOptInfo *rel = path->parent;
	RangeTblEntry *rte;
	int			i;
	ListCell   *lc;

	/*
	 * Forget it if either exact tlist or small tlist is demanded.
	 */
	if (flags & (CP_EXACT_TLIST | CP_SMALL_TLIST))
		return false;

	/*
	 * We can do this for real relation scans, subquery scans, function scans,
	 * values scans, and CTE scans (but not for, eg, joins).
	 */
	if (rel->rtekind != RTE_RELATION &&
		rel->rtekind != RTE_SUBQUERY &&
		rel->rtekind != RTE_FUNCTION &&
		rel->rtekind != RTE_VALUES &&
		rel->rtekind != RTE_TABLEFUNCTION &&
		rel->rtekind != RTE_CTE)
		return false;

	/*
	 * Can't do it with inheritance cases either (mainly because Append
	 * doesn't project; this test may be unnecessary now that
	 * create_append_plan instructs its children to return an exact tlist).
	 */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;

	/*
	 * Can't do it if any system columns or whole-row Vars are requested.
	 * (This could possibly be fixed but would take some fragile assumptions
	 * in setrefs.c, I think.)
	 */
	for (i = rel->min_attr; i <= 0; i++)
	{
		if (!bms_is_empty(rel->attr_needed[i - rel->min_attr]))
			return false;
	}

	/*
	 * Can't do it if the rel is required to emit any placeholder expressions,
	 * either.
	 */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);

		if (bms_nonempty_difference(phinfo->ph_needed, rel->relids) &&
			bms_is_subset(phinfo->ph_eval_at, rel->relids))
			return false;
	}

	/*
	 * Also, can't do it if CP_LABEL_TLIST is specified and path is requested
	 * to emit any sort/group columns that are not simple Vars.  (If they are
	 * simple Vars, they should appear in the physical tlist, and
	 * apply_pathtarget_labeling_to_tlist will take care of getting them
	 * labeled again.)	We also have to check that no two sort/group columns
	 * are the same Var, else that element of the physical tlist would need
	 * conflicting ressortgroupref labels.
	 */
	if ((flags & CP_LABEL_TLIST) && path->pathtarget->sortgrouprefs)
	{
		Bitmapset  *sortgroupatts = NULL;

		i = 0;
		foreach(lc, path->pathtarget->exprs)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			if (path->pathtarget->sortgrouprefs[i])
			{
				if (expr && IsA(expr, Var))
				{
					int			attno = ((Var *) expr)->varattno;

					attno -= FirstLowInvalidHeapAttributeNumber;
					if (bms_is_member(attno, sortgroupatts))
						return false;
					sortgroupatts = bms_add_member(sortgroupatts, attno);
				}
				else
					return false;
			}
			i++;
		}
	}

	/* CDB: Don't use physical tlist if rel has pseudo columns. */
	rte = rt_fetch(rel->relid, root->parse->rtable);
	if (rte->pseudocols)
		return false;

	return true;
}

/*
 * get_gating_quals
 *	  See if there are pseudoconstant quals in a node's quals list
 *
 * If the node's quals list includes any pseudoconstant quals,
 * return just those quals.
 */
static List *
get_gating_quals(PlannerInfo *root, List *quals)
{
	/* No need to look if we know there are no pseudoconstants */
	if (!root->hasPseudoConstantQuals)
		return NIL;

	/* Sort into desirable execution order while still in RestrictInfo form */
	quals = order_qual_clauses(root, quals);

	/* Pull out any pseudoconstant quals from the RestrictInfo list */
	return extract_actual_clauses(quals, true);
}

/*
 * create_gating_plan
 *	  Deal with pseudoconstant qual clauses
 *
 * Add a gating Result node atop the already-built plan.
 */
static Plan *
create_gating_plan(PlannerInfo *root, Path *path, Plan *plan,
				   List *gating_quals)
{
	Plan	   *gplan;

	Assert(gating_quals);

	/*
	 * Since we need a Result node anyway, always return the path's requested
	 * tlist; that's never a wrong choice, even if the parent node didn't ask
	 * for CP_EXACT_TLIST.
	 */
	gplan = (Plan *) make_result(build_path_tlist(root, path),
								 (Node *) gating_quals,
								 plan);

	/*
	 * Notice that we don't change cost or size estimates when doing gating.
	 * The costs of qual eval were already included in the subplan's cost.
	 * Leaving the size alone amounts to assuming that the gating qual will
	 * succeed, which is the conservative estimate for planning upper queries.
	 * We certainly don't want to assume the output size is zero (unless the
	 * gating qual is actually constant FALSE, and that case is dealt with in
	 * clausesel.c).  Interpolating between the two cases is silly, because it
	 * doesn't reflect what will really happen at runtime, and besides which
	 * in most cases we have only a very bad idea of the probability of the
	 * gating qual being true.
	 */
	copy_plan_costsize(gplan, plan);

	return gplan;
}

/*
 * create_join_plan
 *	  Create a join plan for 'best_path' and (recursively) plans for its
 *	  inner and outer paths.
 */
static Plan *
create_join_plan(PlannerInfo *root, JoinPath *best_path)
{
	Plan	   *plan;
	List	   *gating_clauses;
	bool		partition_selector_created;
	List	   *partSelectors;

	/*
	 * Try to inject Partition Selectors.
	 */
	partition_selector_created =
		inject_partition_selectors_for_join(root,
											best_path,
											&partSelectors);

	switch (best_path->path.pathtype)
	{
		case T_MergeJoin:
			plan = (Plan *) create_mergejoin_plan(root,
												  (MergePath *) best_path);
			break;
		case T_HashJoin:
			plan = (Plan *) create_hashjoin_plan(root,
												 (HashPath *) best_path);
			break;
		case T_NestLoop:
			plan = (Plan *) create_nestloop_plan(root,
												 (NestPath *) best_path);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->path.pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

	/*
	 * If we injected a partition selector to the inner side, we must evaluate
	 * the inner side before the outer side, so that the partition selector
	 * can influence the execution of the outer side.
	 */
	Assert(plan->type == best_path->path.pathtype);
	if (partition_selector_created)
		((Join *) plan)->prefetch_inner = true;

	/*
	 * A motion deadlock can also happen when outer and joinqual both contain
	 * motions.  It is not easy to check for joinqual here, so we set the
	 * prefetch_joinqual mark only according to outer motion, and check for
	 * joinqual later in the executor.
	 *
	 * See ExecPrefetchJoinQual() for details.
	 */
	if (best_path->outerjoinpath &&
		best_path->outerjoinpath->motionHazard)
		((Join *) plan)->prefetch_joinqual = true;

	/*
	 * If there are any pseudoconstant clauses attached to this node, insert a
	 * gating Result node that evaluates the pseudoconstants as one-time
	 * quals.
	 */
	gating_clauses = get_gating_quals(root, best_path->joinrestrictinfo);
	if (gating_clauses)
		plan = create_gating_plan(root, (Path *) best_path, plan,
								  gating_clauses);

#ifdef NOT_USED

	/*
	 * * Expensive function pullups may have pulled local predicates * into
	 * this path node.  Put them in the qpqual of the plan node. * JMH,
	 * 6/15/92
	 */
	if (get_loc_restrictinfo(best_path) != NIL)
		set_qpqual((Plan) plan,
				   list_concat(get_qpqual((Plan) plan),
					   get_actual_clauses(get_loc_restrictinfo(best_path))));
#endif

	return plan;
}

/*
 * create_append_plan
 *	  Create an Append plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_append_plan(PlannerInfo *root, AppendPath *best_path)
{
	Append	   *plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	List	   *subplans = NIL;
	ListCell   *subpaths;

	/*
	 * The subpaths list could be empty, if every child was proven empty by
	 * constraint exclusion.  In that case generate a dummy plan that returns
	 * no rows.
	 *
	 * Note that an AppendPath with no members is also generated in certain
	 * cases where there was no appending construct at all, but we know the
	 * relation is empty (see set_dummy_rel_pathlist).
	 */
	if (best_path->subpaths == NIL)
	{
		/* Generate a Result plan with constant-FALSE gating qual */
		Plan	   *plan;

		plan = (Plan *) make_result(tlist,
									(Node *) list_make1(makeBoolConst(false,
																	  false)),
									NULL);

		copy_generic_path_info(plan, (Path *) best_path);

		return plan;
	}

	/* Build the plan for each child */
	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		Plan	   *subplan;

		/* Must insist that all children return the same tlist */
		subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

		subplans = lappend(subplans, subplan);
	}

	/*
	 * XXX ideally, if there's just one child, we'd not bother to generate an
	 * Append node but just return the single child.  At the moment this does
	 * not work because the varno of the child scan plan won't match the
	 * parent-rel Vars it'll be asked to emit.
	 */

	plan = make_append(subplans, tlist);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return (Plan *) plan;
}

/*
 * create_merge_append_plan
 *	  Create a MergeAppend plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path)
{
	MergeAppend *node = makeNode(MergeAppend);
	Plan	   *plan = &node->plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *subplans = NIL;
	ListCell   *subpaths;

	/*
	 * We don't have the actual creation of the MergeAppend node split out
	 * into a separate make_xxx function.  This is because we want to run
	 * prepare_sort_from_pathkeys on it before we do so on the individual
	 * child plans, to make cross-checking the sort info easier.
	 */
	copy_generic_path_info(plan, (Path *) best_path);
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;

	/* Compute sort column info, and adjust MergeAppend's tlist as needed */
	(void) prepare_sort_from_pathkeys(plan, pathkeys,
									  best_path->path.parent->relids,
									  NULL,
									  true,
									  &node->numCols,
									  &node->sortColIdx,
									  &node->sortOperators,
									  &node->collations,
									  &node->nullsFirst,
									  true);

	/*
	 * Now prepare the child plans.  We must apply prepare_sort_from_pathkeys
	 * even to subplans that don't need an explicit sort, to make sure they
	 * are returning the same sort key columns the MergeAppend expects.
	 */
	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		Plan	   *subplan;
		int			numsortkeys;
		AttrNumber *sortColIdx;
		Oid		   *sortOperators;
		Oid		   *collations;
		bool	   *nullsFirst;

		/* Build the child plan */
		/* Must insist that all children return the same tlist */
		subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

		/* Compute sort column info, and adjust subplan's tlist as needed */
		subplan = prepare_sort_from_pathkeys(subplan, pathkeys,
											 subpath->parent->relids,
											 node->sortColIdx,
											 false,
											 &numsortkeys,
											 &sortColIdx,
											 &sortOperators,
											 &collations,
											 &nullsFirst,
											 true);

		/*
		 * Check that we got the same sort key information.  We just Assert
		 * that the sortops match, since those depend only on the pathkeys;
		 * but it seems like a good idea to check the sort column numbers
		 * explicitly, to ensure the tlists really do match up.
		 */
		Assert(numsortkeys == node->numCols);
		if (memcmp(sortColIdx, node->sortColIdx,
				   numsortkeys * sizeof(AttrNumber)) != 0)
			elog(ERROR, "MergeAppend child's targetlist doesn't match MergeAppend");
		Assert(memcmp(sortOperators, node->sortOperators,
					  numsortkeys * sizeof(Oid)) == 0);
		Assert(memcmp(collations, node->collations,
					  numsortkeys * sizeof(Oid)) == 0);
		Assert(memcmp(nullsFirst, node->nullsFirst,
					  numsortkeys * sizeof(bool)) == 0);

		/* Now, insert a Sort node if subplan isn't sufficiently ordered */
		if (!pathkeys_contained_in(pathkeys, subpath->pathkeys))
		{
			Sort	   *sort = make_sort(subplan, numsortkeys,
										 sortColIdx, sortOperators,
										 collations, nullsFirst);

			label_sort_with_costsize(root, sort, best_path->limit_tuples);
			subplan = (Plan *) sort;
		}

		subplans = lappend(subplans, subplan);
	}

	node->mergeplans = subplans;

	return (Plan *) node;
}

/*
 * create_result_plan
 *	  Create a Result plan for 'best_path'.
 *	  This is only used for degenerate cases, such as a query with an empty
 *	  jointree.
 *
 *	  Returns a Plan node.
 */
static Result *
create_result_plan(PlannerInfo *root, ResultPath *best_path)
{
	Result	   *plan;
	List	   *tlist;
	List	   *quals;

	tlist = build_path_tlist(root, &best_path->path);

	/* best_path->quals is just bare clauses */
	quals = order_qual_clauses(root, best_path->quals);

	plan = make_result(tlist, (Node *) quals, NULL);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_material_plan
 *	  Create a Material plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Material *
create_material_plan(PlannerInfo *root, MaterialPath *best_path, int flags)
{
	Material   *plan;
	Plan	   *subplan;

	/*
	 * We don't want any excess columns in the materialized tuples, so request
	 * a smaller tlist.  Otherwise, since Material doesn't project, tlist
	 * requirements pass through.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_SMALL_TLIST);

	plan = make_material(subplan);

	plan->cdb_strict = best_path->cdb_strict;
	plan->cdb_shield_child_from_rescans = best_path->cdb_shield_child_from_rescans;

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_unique_plan
 *	  Create a Unique plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_unique_plan(PlannerInfo *root, UniquePath *best_path, int flags)
{
	Plan	   *plan;
	Plan	   *subplan;
	List	   *in_operators;
	List	   *uniq_exprs;
	List	   *newtlist;
	int			nextresno;
	bool		newitems;
	int			numGroupCols;
	AttrNumber *groupColIdx;
	int			groupColPos;
	ListCell   *l;

	/* Unique doesn't project, so tlist requirements pass through */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	/* Return naked subplan if we don't need to do any actual unique-ifying */
	if (best_path->umethod == UNIQUE_PATH_NOOP)
		return subplan;

	/*
	 * As constructed, the subplan has a "flat" tlist containing just the Vars
	 * needed here and at upper levels.  The values we are supposed to
	 * unique-ify may be expressions in these variables.  We have to add any
	 * such expressions to the subplan's tlist.
	 *
	 * The subplan may have a "physical" tlist if it is a simple scan plan. If
	 * we're going to sort, this should be reduced to the regular tlist, so
	 * that we don't sort more data than we need to.  For hashing, the tlist
	 * should be left as-is if we don't need to add any expressions; but if we
	 * do have to add expressions, then a projection step will be needed at
	 * runtime anyway, so we may as well remove unneeded items. Therefore
	 * newtlist starts from build_path_tlist() not just a copy of the
	 * subplan's tlist; and we don't install it into the subplan unless we are
	 * sorting or stuff has to be added.
	 */
	in_operators = best_path->in_operators;
	uniq_exprs = best_path->uniq_exprs;

	/* initialize modified subplan tlist as just the "required" vars */
	newtlist = build_path_tlist(root, &best_path->path);
	nextresno = list_length(newtlist) + 1;
	newitems = false;

	foreach(l, uniq_exprs)
	{
		Node	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlist_member(uniqexpr, newtlist);
		if (!tle)
		{
			tle = makeTargetEntry((Expr *) uniqexpr,
								  nextresno,
								  NULL,
								  false);
			newtlist = lappend(newtlist, tle);
			nextresno++;
			newitems = true;
		}
	}

	if (newitems || best_path->umethod == UNIQUE_PATH_SORT)
	{
		/*
		 * If the top plan node can't do projections and its existing target
		 * list isn't already what we need, we need to add a Result node to
		 * help it along.
		 */
		subplan = plan_pushdown_tlist(root, subplan, newtlist);
	}

	/*
	 * Build control information showing which subplan output columns are to
	 * be examined by the grouping step.  Unfortunately we can't merge this
	 * with the previous loop, since we didn't then know which version of the
	 * subplan tlist we'd end up using.
	 */
	newtlist = subplan->targetlist;
	numGroupCols = list_length(uniq_exprs);
	groupColIdx = (AttrNumber *) palloc(numGroupCols * sizeof(AttrNumber));

	groupColPos = 0;
	foreach(l, uniq_exprs)
	{
		Node	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlist_member(uniqexpr, newtlist);
		if (!tle)				/* shouldn't happen */
			elog(ERROR, "failed to find unique expression in subplan tlist");
		groupColIdx[groupColPos++] = tle->resno;
	}

	if (best_path->umethod == UNIQUE_PATH_HASH)
	{
		Oid		   *groupOperators;

		/*
		 * Get the hashable equality operators for the Agg node to use.
		 * Normally these are the same as the IN clause operators, but if
		 * those are cross-type operators then the equality operators are the
		 * ones for the IN clause operators' RHS datatype.
		 */
		groupOperators = (Oid *) palloc(numGroupCols * sizeof(Oid));
		groupColPos = 0;
		foreach(l, in_operators)
		{
			Oid			in_oper = lfirst_oid(l);
			Oid			eq_oper;

			if (!get_compatible_hash_operators(in_oper, NULL, &eq_oper))
				elog(ERROR, "could not find compatible hash operator for operator %u",
					 in_oper);
			groupOperators[groupColPos++] = eq_oper;
		}

		/*
		 * Since the Agg node is going to project anyway, we can give it the
		 * minimum output tlist, without any stuff we might have added to the
		 * subplan tlist.
		 */
		plan = (Plan *) make_agg(build_path_tlist(root, &best_path->path),
								 NIL,
								 AGG_HASHED,
								 AGGSPLIT_SIMPLE,
								 false, /* streaming */
								 numGroupCols,
								 groupColIdx,
								 groupOperators,
								 NIL,
								 NIL,
								 best_path->path.rows,
								 subplan);
	}
	else
	{
		List	   *sortList = NIL;
		Sort	   *sort;

		/* Create an ORDER BY list to sort the input compatibly */
		groupColPos = 0;
		foreach(l, in_operators)
		{
			Oid			in_oper = lfirst_oid(l);
			Oid			sortop;
			Oid			eqop;
			TargetEntry *tle;
			SortGroupClause *sortcl;

			sortop = get_ordering_op_for_equality_op(in_oper, false);
			if (!OidIsValid(sortop))	/* shouldn't happen */
				elog(ERROR, "could not find ordering operator for equality operator %u",
					 in_oper);

			/*
			 * The Unique node will need equality operators.  Normally these
			 * are the same as the IN clause operators, but if those are
			 * cross-type operators then the equality operators are the ones
			 * for the IN clause operators' RHS datatype.
			 */
			eqop = get_equality_op_for_ordering_op(sortop, NULL);
			if (!OidIsValid(eqop))		/* shouldn't happen */
				elog(ERROR, "could not find equality operator for ordering operator %u",
					 sortop);

			tle = get_tle_by_resno(subplan->targetlist,
								   groupColIdx[groupColPos]);
			Assert(tle != NULL);

			sortcl = makeNode(SortGroupClause);
			sortcl->tleSortGroupRef = assignSortGroupRef(tle,
														 subplan->targetlist);
			sortcl->eqop = eqop;
			sortcl->sortop = sortop;
			sortcl->nulls_first = false;
			sortcl->hashable = false;	/* no need to make this accurate */
			sortList = lappend(sortList, sortcl);
			groupColPos++;
		}
		sort = make_sort_from_sortclauses(sortList, subplan);
		label_sort_with_costsize(root, sort, -1.0);
		plan = (Plan *) make_unique_from_sortclauses((Plan *) sort, sortList);
	}

	/* Copy cost data from Path to Plan */
	copy_generic_path_info(plan, &best_path->path);

	return plan;
}

/*
 * create_gather_plan
 *
 *	  Create a Gather plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Gather *
create_gather_plan(PlannerInfo *root, GatherPath *best_path)
{
	Gather	   *gather_plan;
	Plan	   *subplan;
	List	   *tlist;

	/*
	 * Although the Gather node can project, we prefer to push down such work
	 * to its child node, so demand an exact tlist from the child.
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_EXACT_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	gather_plan = make_gather(tlist,
							  NIL,
							  best_path->path.parallel_workers,
							  best_path->single_copy,
							  subplan);

	copy_generic_path_info(&gather_plan->plan, &best_path->path);

	/* use parallel mode for parallel plans. */
	root->glob->parallelModeNeeded = true;

	return gather_plan;
}

/*
 * create_projection_plan
 *
 *	  Create a plan tree to do a projection step and (recursively) plans
 *	  for its subpaths.  We may need a Result node for the projection,
 *	  but sometimes we can just let the subplan do the work.
 */
static Plan *
create_projection_plan(PlannerInfo *root, ProjectionPath *best_path)
{
	Plan	   *plan;
	Plan	   *subplan;
	List	   *tlist;

	/* Since we intend to project, we don't need to constrain child tlist */
	subplan = create_plan_recurse(root, best_path->subpath, 0);

	tlist = build_path_tlist(root, &best_path->path);

	/*
	 * We might not really need a Result node here, either because the subplan
	 * can project or because it's returning the right list of expressions
	 * anyway.  Usually create_projection_path will have detected that and set
	 * dummypp if we don't need a Result; but its decision can't be final,
	 * because some createplan.c routines change the tlists of their nodes.
	 * (An example is that create_merge_append_plan might add resjunk sort
	 * columns to a MergeAppend.)  So we have to recheck here.  If we do
	 * arrive at a different answer than create_projection_path did, we'll
	 * have made slightly wrong cost estimates; but label the plan with the
	 * cost estimates we actually used, not "corrected" ones.  (XXX this could
	 * be cleaned up if we moved more of the sortcolumn setup logic into Path
	 * creation, but that would add expense to creating Paths we might end up
	 * not using.)
	 */
	if (!best_path->cdb_restrict_clauses &&
		(is_projection_capable_path(best_path->subpath) ||
		 tlist_same_exprs(tlist, subplan->targetlist)))
	{
		/* Don't need a separate Result, just assign tlist to subplan */
		plan = subplan;
		plan->targetlist = tlist;

		/* Label plan with the estimated costs we actually used */
		plan->startup_cost = best_path->path.startup_cost;
		plan->total_cost = best_path->path.total_cost;
		plan->plan_rows = best_path->path.rows;
		plan->plan_width = best_path->path.pathtarget->width;
		/* ... but be careful not to munge subplan's parallel-aware flag */
	}
	else
	{
		List	   *scan_clauses = NIL;
		List	   *pseudoconstants = NIL;

		if (best_path->cdb_restrict_clauses)
		{
			List	   *all_clauses = best_path->cdb_restrict_clauses;

			/* Replace any outer-relation variables with nestloop params */
			if (best_path->path.param_info)
			{
				all_clauses = (List *)
					replace_nestloop_params(root, (Node *) all_clauses);
			}

			/* Sort clauses into best execution order */
			all_clauses = order_qual_clauses(root, all_clauses);

			/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
			scan_clauses = extract_actual_clauses(all_clauses, false);

			/* but we actually also want the pseudoconstants */
			pseudoconstants = extract_actual_clauses(all_clauses, true);
		}

		/* We need a Result node */
		plan = (Plan *) make_result(tlist, (Node *) pseudoconstants, subplan);
		plan->qual = scan_clauses;

		copy_generic_path_info(plan, (Path *) best_path);
	}

	return plan;
}

/*
 * inject_projection_plan
 *	  Insert a Result node to do a projection step.
 *
 * This is used in a few places where we decide on-the-fly that we need a
 * projection step as part of the tree generated for some Path node.
 * We should try to get rid of this in favor of doing it more honestly.
 */
static Plan *
inject_projection_plan(Plan *subplan, List *tlist)
{
	Plan	   *plan;

	plan = (Plan *) make_result(tlist, NULL, subplan);

	/*
	 * In principle, we should charge tlist eval cost plus cpu_per_tuple per
	 * row for the Result node.  But the former has probably been factored in
	 * already and the latter was not accounted for during Path construction,
	 * so being formally correct might just make the EXPLAIN output look less
	 * consistent not more so.  Hence, just copy the subplan's cost.
	 */
	copy_plan_costsize(plan, subplan);

	return plan;
}

/*
 * create_sort_plan
 *
 *	  Create a Sort plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Sort *
create_sort_plan(PlannerInfo *root, SortPath *best_path, int flags)
{
	Sort	   *plan;
	Plan	   *subplan;

	/*
	 * We don't want any excess columns in the sorted tuples, so request a
	 * smaller tlist.  Otherwise, since Sort doesn't project, tlist
	 * requirements pass through.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_SMALL_TLIST);

	plan = make_sort_from_pathkeys(subplan, best_path->path.pathkeys,
								   false /* GPDB_96_MERGE_FIXME: is 'false' correct here? */);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_upper_unique_plan
 *
 *	  Create a Unique plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Unique *
create_upper_unique_plan(PlannerInfo *root, UpperUniquePath *best_path, int flags)
{
	Unique	   *plan;
	Plan	   *subplan;

	/*
	 * Unique doesn't project, so tlist requirements pass through; moreover we
	 * need grouping columns to be labeled.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_LABEL_TLIST);

	plan = make_unique_from_pathkeys(subplan,
									 best_path->path.pathkeys,
									 best_path->numkeys);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_agg_plan
 *
 *	  Create an Agg plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Agg *
create_agg_plan(PlannerInfo *root, AggPath *best_path)
{
	Agg		   *plan;
	Plan	   *subplan;
	List	   *tlist;
	List	   *quals;

	/*
	 * Agg can project, so no need to be terribly picky about child tlist, but
	 * we do need grouping columns to be available
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	quals = order_qual_clauses(root, best_path->qual);

	plan = make_agg(tlist, quals,
					best_path->aggstrategy,
					best_path->aggsplit,
					best_path->streaming,
					list_length(best_path->groupClause),
					extract_grouping_cols(best_path->groupClause,
										  subplan->targetlist),
					extract_grouping_ops(best_path->groupClause),
					NIL,
					NIL,
					best_path->numGroups,
					subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * Given a groupclause for a collection of grouping sets, produce the
 * corresponding groupColIdx.
 *
 * root->grouping_map maps the tleSortGroupRef to the actual column position in
 * the input tuple. So we get the ref from the entries in the groupclause and
 * look them up there.
 */
static AttrNumber *
remap_groupColIdx(PlannerInfo *root, List *groupClause)
{
	AttrNumber *grouping_map = root->grouping_map;
	AttrNumber *new_grpColIdx;
	ListCell   *lc;
	int			i;

	Assert(grouping_map);

	new_grpColIdx = palloc0(sizeof(AttrNumber) * list_length(groupClause));

	i = 0;
	foreach(lc, groupClause)
	{
		SortGroupClause *clause = lfirst(lc);

		new_grpColIdx[i++] = grouping_map[clause->tleSortGroupRef];
	}

	return new_grpColIdx;
}

/*
 * create_groupingsets_plan
 *	  Create a plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  What we emit is an Agg plan with some vestigial Agg and Sort nodes
 *	  hanging off the side.  The top Agg implements the last grouping set
 *	  specified in the GroupingSetsPath, and any additional grouping sets
 *	  each give rise to a subsidiary Agg and Sort node in the top Agg's
 *	  "chain" list.  These nodes don't participate in the plan directly,
 *	  but they are a convenient way to represent the required data for
 *	  the extra steps.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_groupingsets_plan(PlannerInfo *root, GroupingSetsPath *best_path)
{
	Agg		   *plan;
	Plan	   *subplan;
	List	   *rollup_groupclauses = best_path->rollup_groupclauses;
	List	   *rollup_lists = best_path->rollup_lists;
	AttrNumber *grouping_map;
	int			maxref;
	List	   *chain;
	ListCell   *lc,
			   *lc2;

	/* Shouldn't get here without grouping sets */
	Assert(root->parse->groupingSets);
	Assert(rollup_lists != NIL);
	Assert(list_length(rollup_lists) == list_length(rollup_groupclauses));

	/*
	 * Agg can project, so no need to be terribly picky about child tlist, but
	 * we do need grouping columns to be available
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	/*
	 * Compute the mapping from tleSortGroupRef to column index in the child's
	 * tlist.  First, identify max SortGroupRef in groupClause, for array
	 * sizing.
	 */
	maxref = 0;
	foreach(lc, root->parse->groupClause)
	{
		SortGroupClause *gc = (SortGroupClause *) lfirst(lc);

		if (gc->tleSortGroupRef > maxref)
			maxref = gc->tleSortGroupRef;
	}

	grouping_map = (AttrNumber *) palloc0((maxref + 1) * sizeof(AttrNumber));

	/* Now look up the column numbers in the child's tlist */
	foreach(lc, root->parse->groupClause)
	{
		SortGroupClause *gc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(gc, subplan->targetlist);

		grouping_map[gc->tleSortGroupRef] = tle->resno;
	}

	/*
	 * During setrefs.c, we'll need the grouping_map to fix up the cols lists
	 * in GroupingFunc nodes.  Save it for setrefs.c to use.
	 *
	 * This doesn't work if we're in an inheritance subtree (see notes in
	 * create_modifytable_plan).  Fortunately we can't be because there would
	 * never be grouping in an UPDATE/DELETE; but let's Assert that.
	 */
	Assert(!root->hasInheritedTarget);
	Assert(root->grouping_map == NULL);
	root->grouping_map = grouping_map;
	root->grouping_map_size = maxref + 1;

	/*
	 * Generate the side nodes that describe the other sort and group
	 * operations besides the top one.  Note that we don't worry about putting
	 * accurate cost estimates in the side nodes; only the topmost Agg node's
	 * costs will be shown by EXPLAIN.
	 */
	chain = NIL;
	if (list_length(rollup_groupclauses) > 1)
	{
		forboth(lc, rollup_groupclauses, lc2, rollup_lists)
		{
			List	   *groupClause = (List *) lfirst(lc);
			List	   *gsets = (List *) lfirst(lc2);
			AttrNumber *new_grpColIdx;
			Plan	   *sort_plan;
			Plan	   *agg_plan;

			/* We want to iterate over all but the last rollup list elements */
			if (lnext(lc) == NULL)
				break;

			new_grpColIdx = remap_groupColIdx(root, groupClause);

			sort_plan = (Plan *)
				make_sort_from_groupcols(groupClause,
										 new_grpColIdx,
										 subplan);

			agg_plan = (Plan *) make_agg(NIL,
										 NIL,
										 AGG_SORTED,
										 AGGSPLIT_SIMPLE,
										 false, /* streaming */
									   list_length((List *) linitial(gsets)),
										 new_grpColIdx,
										 extract_grouping_ops(groupClause),
										 gsets,
										 NIL,
										 0,		/* numGroups not needed */
										 sort_plan);

			/*
			 * Nuke stuff we don't need to avoid bloating debug output.
			 */
			sort_plan->targetlist = NIL;
			sort_plan->lefttree = NULL;

			chain = lappend(chain, agg_plan);
		}
	}

	/*
	 * Now make the final Agg node
	 */
	{
		List	   *groupClause = (List *) llast(rollup_groupclauses);
		List	   *gsets = (List *) llast(rollup_lists);
		AttrNumber *top_grpColIdx;
		int			numGroupCols;

		top_grpColIdx = remap_groupColIdx(root, groupClause);

		numGroupCols = list_length((List *) linitial(gsets));

		plan = make_agg(build_path_tlist(root, &best_path->path),
						best_path->qual,
						(numGroupCols > 0) ? AGG_SORTED : AGG_PLAIN,
						AGGSPLIT_SIMPLE,
						false, /* streaming */
						numGroupCols,
						top_grpColIdx,
						extract_grouping_ops(groupClause),
						gsets,
						chain,
						0,		/* numGroups not needed */
						subplan);

		/* Copy cost data from Path to Plan */
		copy_generic_path_info(&plan->plan, &best_path->path);
	}

	return (Plan *) plan;
}

/*
 * create_minmaxagg_plan
 *
 *	  Create a Result plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Result *
create_minmaxagg_plan(PlannerInfo *root, MinMaxAggPath *best_path)
{
	Result	   *plan;
	List	   *tlist;
	ListCell   *lc;

	/* Prepare an InitPlan for each aggregate's subquery. */
	foreach(lc, best_path->mmaggregates)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);
		PlannerInfo *subroot = mminfo->subroot;
		Query	   *subparse = subroot->parse;
		Plan	   *plan;

		mminfo->path = cdbllize_adjust_init_plan_path(subroot, mminfo->path);

		/*
		 * Generate the plan for the subquery. We already have a Path, but we
		 * have to convert it to a Plan and attach a LIMIT node above it.
		 * Since we are entering a different planner context (subroot),
		 * recurse to create_plan not create_plan_recurse.
		 */
		plan = create_plan(subroot, mminfo->path, root->curSlice);

		plan = (Plan *) make_limit(plan,
								   subparse->limitOffset,
								   subparse->limitCount);
		plan->flow = plan->lefttree->flow;

		/* Must apply correct cost/width data to Limit node */
		plan->startup_cost = mminfo->path->startup_cost;
		plan->total_cost = mminfo->pathcost;
		plan->plan_rows = 1;
		plan->plan_width = mminfo->path->pathtarget->width;
		plan->parallel_aware = false;

		/* Convert the plan into an InitPlan in the outer query. */
		SS_make_initplan_from_plan(root, subroot, plan, root->curSlice, mminfo->param);
	}

	/* Generate the output plan --- basically just a Result */
	tlist = build_path_tlist(root, &best_path->path);

	plan = make_result(tlist, (Node *) best_path->quals, NULL);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	/*
	 * During setrefs.c, we'll need to replace references to the Agg nodes
	 * with InitPlan output params.  (We can't just do that locally in the
	 * MinMaxAgg node, because path nodes above here may have Agg references
	 * as well.)  Save the mmaggregates list to tell setrefs.c to do that.
	 *
	 * This doesn't work if we're in an inheritance subtree (see notes in
	 * create_modifytable_plan).  Fortunately we can't be because there would
	 * never be aggregates in an UPDATE/DELETE; but let's Assert that.
	 */
	Assert(!root->hasInheritedTarget);
	Assert(root->minmax_aggs == NIL);
	root->minmax_aggs = best_path->mmaggregates;

	return plan;
}

/*
 * create_windowagg_plan
 *
 *	  Create a WindowAgg plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static WindowAgg *
create_windowagg_plan(PlannerInfo *root, WindowAggPath *best_path)
{
	WindowAgg  *plan;
	WindowClause *wc = best_path->winclause;
	Plan	   *subplan;
	List	   *tlist;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;
	int			partNumCols;
	AttrNumber *partColIdx;
	Oid		   *partOperators;
	int			ordNumCols;
	AttrNumber *ordColIdx;
	Oid		   *ordOperators;
	int			firstOrderCol = 0;
	Oid			firstOrderCmpOperator = InvalidOid;
	bool		firstOrderNullsFirst = false;

	/*
	 * WindowAgg can project, so no need to be terribly picky about child
	 * tlist, but we do need grouping columns to be available
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	/*
	 * We shouldn't need to actually sort, but it's convenient to use
	 * prepare_sort_from_pathkeys to identify the input's sort columns.
	 */
	subplan = prepare_sort_from_pathkeys(subplan,
										 best_path->winpathkeys,
										 NULL,
										 NULL,
										 false,
										 &numsortkeys,
										 &sortColIdx,
										 &sortOperators,
										 &collations,
										 &nullsFirst,
										 true);

	/* Now deconstruct that into partition and ordering portions */
	get_column_info_for_window(root,
							   wc,
							   subplan->targetlist,
							   numsortkeys,
							   sortColIdx,
							   &partNumCols,
							   &partColIdx,
							   &partOperators,
							   &ordNumCols,
							   &ordColIdx,
							   &ordOperators);

	if (wc->orderClause)
	{
		SortGroupClause *sortcl = (SortGroupClause *) linitial(wc->orderClause);
		ListCell	*l_tle;

		firstOrderCol = 0;
		foreach(l_tle, subplan->targetlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(l_tle);

			firstOrderCol++;
			if (sortcl->tleSortGroupRef == tle->ressortgroupref)
				break;
		}
		if (!l_tle)
			elog(ERROR, "failed to locate ORDER BY column");

		firstOrderCmpOperator = sortcl->sortop;
		firstOrderNullsFirst = sortcl->nulls_first;
	}

	/* And finally we can make the WindowAgg node */
	plan = make_windowagg(tlist,
						  wc->winref,
						  partNumCols,
						  partColIdx,
						  partOperators,
						  ordNumCols,
						  ordColIdx,
						  ordOperators,
						  firstOrderCol,
						  firstOrderCmpOperator,
						  firstOrderNullsFirst,
						  wc->frameOptions,
						  wc->startOffset,
						  wc->endOffset,
						  subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * get_column_info_for_window
 *		Get the partitioning/ordering column numbers and equality operators
 *		for a WindowAgg node.
 *
 * This depends on the behavior of planner.c's make_pathkeys_for_window!
 *
 * We are given the target WindowClause and an array of the input column
 * numbers associated with the resulting pathkeys.  In the easy case, there
 * are the same number of pathkey columns as partitioning + ordering columns
 * and we just have to copy some data around.  However, it's possible that
 * some of the original partitioning + ordering columns were eliminated as
 * redundant during the transformation to pathkeys.  (This can happen even
 * though the parser gets rid of obvious duplicates.  A typical scenario is a
 * window specification "PARTITION BY x ORDER BY y" coupled with a clause
 * "WHERE x = y" that causes the two sort columns to be recognized as
 * redundant.)	In that unusual case, we have to work a lot harder to
 * determine which keys are significant.
 *
 * The method used here is a bit brute-force: add the sort columns to a list
 * one at a time and note when the resulting pathkey list gets longer.  But
 * it's a sufficiently uncommon case that a faster way doesn't seem worth
 * the amount of code refactoring that'd be needed.
 */
static void
get_column_info_for_window(PlannerInfo *root, WindowClause *wc, List *tlist,
						   int numSortCols, AttrNumber *sortColIdx,
						   int *partNumCols,
						   AttrNumber **partColIdx,
						   Oid **partOperators,
						   int *ordNumCols,
						   AttrNumber **ordColIdx,
						   Oid **ordOperators)
{
	int			numPart = list_length(wc->partitionClause);
	int			numOrder = list_length(wc->orderClause);

	if (numSortCols == numPart + numOrder)
	{
		/* easy case */
		*partNumCols = numPart;
		*partColIdx = sortColIdx;
		*partOperators = extract_grouping_ops(wc->partitionClause);
		*ordNumCols = numOrder;
		*ordColIdx = sortColIdx + numPart;
		*ordOperators = extract_grouping_ops(wc->orderClause);
	}
	else
	{
		List	   *sortclauses;
		List	   *pathkeys;
		int			scidx;
		ListCell   *lc;

		/* first, allocate what's certainly enough space for the arrays */
		*partNumCols = 0;
		*partColIdx = (AttrNumber *) palloc(numPart * sizeof(AttrNumber));
		*partOperators = (Oid *) palloc(numPart * sizeof(Oid));
		*ordNumCols = 0;
		*ordColIdx = (AttrNumber *) palloc(numOrder * sizeof(AttrNumber));
		*ordOperators = (Oid *) palloc(numOrder * sizeof(Oid));
		sortclauses = NIL;
		pathkeys = NIL;
		scidx = 0;
		foreach(lc, wc->partitionClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
			List	   *new_pathkeys;

			sortclauses = lappend(sortclauses, sgc);
			new_pathkeys = make_pathkeys_for_sortclauses(root,
														 sortclauses,
														 tlist);
			if (list_length(new_pathkeys) > list_length(pathkeys))
			{
				/* this sort clause is actually significant */
				(*partColIdx)[*partNumCols] = sortColIdx[scidx++];
				(*partOperators)[*partNumCols] = sgc->eqop;
				(*partNumCols)++;
				pathkeys = new_pathkeys;
			}
		}
		foreach(lc, wc->orderClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
			List	   *new_pathkeys;

			sortclauses = lappend(sortclauses, sgc);
			new_pathkeys = make_pathkeys_for_sortclauses(root,
														 sortclauses,
														 tlist);
			if (list_length(new_pathkeys) > list_length(pathkeys))
			{
				/* this sort clause is actually significant */
				(*ordColIdx)[*ordNumCols] = sortColIdx[scidx++];
				(*ordOperators)[*ordNumCols] = sgc->eqop;
				(*ordNumCols)++;
				pathkeys = new_pathkeys;
			}
		}
		/* complain if we didn't eat exactly the right number of sort cols */
		if (scidx != numSortCols)
			elog(ERROR, "failed to deconstruct sort operators into partitioning/ordering operators");
	}
}

/*
 * create_setop_plan
 *
 *	  Create a SetOp plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static SetOp *
create_setop_plan(PlannerInfo *root, SetOpPath *best_path, int flags)
{
	SetOp	   *plan;
	Plan	   *subplan;
	long		numGroups;

	/*
	 * SetOp doesn't project, so tlist requirements pass through; moreover we
	 * need grouping columns to be labeled.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_LABEL_TLIST);

	/* Convert numGroups to long int --- but 'ware overflow! */
	numGroups = (long) Min(best_path->numGroups, (double) LONG_MAX);

	plan = make_setop(best_path->cmd,
					  best_path->strategy,
					  subplan,
					  best_path->distinctList,
					  best_path->flagColIdx,
					  best_path->firstFlag,
					  numGroups);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_recursiveunion_plan
 *
 *	  Create a RecursiveUnion plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static RecursiveUnion *
create_recursiveunion_plan(PlannerInfo *root, RecursiveUnionPath *best_path)
{
	RecursiveUnion *plan;
	Plan	   *leftplan;
	Plan	   *rightplan;
	List	   *tlist;
	long		numGroups;

	/* Need both children to produce same tlist, so force it */
	leftplan = create_plan_recurse(root, best_path->leftpath, CP_EXACT_TLIST);
	rightplan = create_plan_recurse(root, best_path->rightpath, CP_EXACT_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	/* Convert numGroups to long int --- but 'ware overflow! */
	numGroups = (long) Min(best_path->numGroups, (double) LONG_MAX);

	plan = make_recursive_union(tlist,
								leftplan,
								rightplan,
								best_path->wtParam,
								best_path->distinctList,
								numGroups);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_lockrows_plan
 *
 *	  Create a LockRows plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static LockRows *
create_lockrows_plan(PlannerInfo *root, LockRowsPath *best_path,
					 int flags)
{
	LockRows   *plan;
	Plan	   *subplan;

	/* LockRows doesn't project, so tlist requirements pass through */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	plan = make_lockrows(subplan, best_path->rowMarks, best_path->epqParam);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_modifytable_plan
 *	  Create a ModifyTable plan for 'best_path'.
 *
 *	  Returns a Plan node.
 */
static ModifyTable *
create_modifytable_plan(PlannerInfo *root, ModifyTablePath *best_path)
{
	ModifyTable *plan;
	List	   *subplans = NIL;
	ListCell   *subpaths,
			   *subroots;
	ListCell   *is_split_updates;

	/* Build the plan for each input path */
	forthree(subpaths, best_path->subpaths,
			 subroots, best_path->subroots,
			 is_split_updates, best_path->is_split_updates)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		PlannerInfo *subroot = (PlannerInfo *) lfirst(subroots);
		bool		is_split_update = (bool) lfirst_int(is_split_updates);
		Plan	   *subplan;
		RangeTblEntry *rte = planner_rt_fetch(best_path->nominalRelation, root);
		PlanSlice  *save_curSlice = subroot->curSlice;

		subroot->curSlice = root->curSlice;

		/* Try the Single-Row-Insert optimization first. */
		subplan = cdbpathtoplan_create_sri_plan(rte, subroot, subpath, CP_EXACT_TLIST);

		/*
		 * In an inherited UPDATE/DELETE, reference the per-child modified
		 * subroot while creating Plans from Paths for the child rel.  This is
		 * a kluge, but otherwise it's too hard to ensure that Plan creation
		 * functions (particularly in FDWs) don't depend on the contents of
		 * "root" matching what they saw at Path creation time.  The main
		 * downside is that creation functions for Plans that might appear
		 * below a ModifyTable cannot expect to modify the contents of "root"
		 * and have it "stick" for subsequent processing such as setrefs.c.
		 * That's not great, but it seems better than the alternative.
		 */
		if (!subplan)
		{
			subplan = create_plan_recurse(subroot, subpath, CP_EXACT_TLIST);

			/*
			 * Transfer resname/resjunk labeling, too, to keep executor happy.
			 * But not if it's a Split Update. A Split Update contains an extra
			 * DMLActionExpr column in its target list, so it doesn't match
			 * subroot->processed_tlist. The code to create the Split Update node
			 * takes care to label junk columns correctly, instead.
			 */
			if (!is_split_update)
				apply_tlist_labeling(subplan->targetlist, subroot->processed_tlist);
		}

		subplans = lappend(subplans, subplan);

		subroot->curSlice = save_curSlice;
	}

	plan = make_modifytable(root,
							best_path->operation,
							best_path->canSetTag,
							best_path->nominalRelation,
							best_path->resultRelations,
							subplans,
							best_path->withCheckOptionLists,
							best_path->returningLists,
							best_path->is_split_updates,
							best_path->rowMarks,
							best_path->onconflict,
							best_path->epqParam);

	copy_generic_path_info(&plan->plan, &best_path->path);

	if (list_length(plan->resultRelations) > 0 && Gp_role == GP_ROLE_DISPATCH)
	{
		GpPolicyType policyType = POLICYTYPE_ENTRY;
		bool		isfirst = true;
		ListCell   *lc;

		foreach (lc, plan->resultRelations)
		{
			int			idx = lfirst_int(lc);
			Oid			reloid = planner_rt_fetch(idx, root)->relid;
			GpPolicy   *policy = GpPolicyFetch(reloid);

			/*
			 * We cannot update tables on segments and on the entry DB in the
			 * same process.
			 */
			if (isfirst)
				policyType = policy->ptype;
			else
			{
				if (policy->ptype != policyType)
					elog(ERROR, "ModifyTable mixes distributed and entry-only tables");
			}

			if (policyType != POLICYTYPE_ENTRY)
			{
				if (isfirst)
				{
					root->curSlice->gangType = GANGTYPE_PRIMARY_WRITER;
					root->curSlice->numsegments = policy->numsegments;
				}
				else
				{
					Assert(root->curSlice->gangType == GANGTYPE_PRIMARY_WRITER);
					root->curSlice->numsegments =
						Max(root->curSlice->numsegments, policy->numsegments);
				}
			}
			isfirst = false;
		}
	}

	return plan;
}

/*
 * create_limit_plan
 *
 *	  Create a Limit plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Limit *
create_limit_plan(PlannerInfo *root, LimitPath *best_path, int flags)
{
	Limit	   *plan;
	Plan	   *subplan;

	/* Limit doesn't project, so tlist requirements pass through */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	plan = make_limit(subplan,
					  best_path->limitOffset,
					  best_path->limitCount);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}


/*
 * create_motion_plan
 */
Plan *
create_motion_plan(PlannerInfo *root, CdbMotionPath *path)
{
	Motion	   *motion;
	Path	   *subpath = path->subpath;
	Plan	   *subplan;
	Relids		save_curOuterRels = root->curOuterRels;
	List	   *save_curOuterParams = root->curOuterParams;
	int			before_numMotions;
	PlanSlice  *save_curSlice = root->curSlice;
	PlanSlice  *sendSlice;

	/*
	 * singleQE-->entry:  Elide the motion.  The subplan will run in the same
	 * process with its parent: either the qDisp (if it is a top slice) or a
	 * singleton gang on the entry db (otherwise).
	 */
	if (CdbPathLocus_IsEntry(path->path.locus) &&
		CdbPathLocus_IsSingleQE(subpath->locus))
	{
		/* Push the MotionPath's locus down onto subpath. */
		subpath->locus = path->path.locus;

		subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

		return subplan;
	}

	/*
	 * Remember old value of 'numMotions', before recursing. By comparing
	 * the old value with the new value after the call returns, we know
	 * if there were any Motions in the subtree.
	 */
	before_numMotions = root->numMotions;

	root->curOuterRels = NULL;
	root->curOuterParams = NIL;

	/*
	 * Set up a new slice struct, to represent the sending slice.
	 */
	sendSlice = palloc0(sizeof(PlanSlice));
	sendSlice->gangType = GANGTYPE_PRIMARY_READER;
	sendSlice->sliceIndex = -1;

	root->curSlice = sendSlice;

	subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

	root->curSlice = save_curSlice;

	/* Check we successfully assigned all NestLoopParams to plan nodes */
	if (root->curOuterParams != NIL)
		elog(ERROR, "failed to assign all NestLoopParams to plan nodes");

	/*
	 * Reset plan_params to ensure param IDs used for nestloop params are not
	 * re-used later
	 */
	root->plan_params = NIL;

	/*
	 * Elide explicit motion, if the subplan doesn't contain any motions.
	 *
	 * The idea is that if an Explicit Motion has no Motions underneath it,
	 * then the row to update must originate from the same segment, and no
	 * Motion is needed. This is quite conservative, we could elide the motion
	 * even if there are Motions, as long as they are not between the scan
	 * on the target table and the ModifyTable.
	 *
	 * A SplitUpdate also computes the target segment ID, based on other columns,
	 * so we treat it the same as a Motion node for this purpose.
	 */
	if (root->numMotions == before_numMotions && path->is_explicit_motion)
	{
		root->curOuterRels = save_curOuterRels;

		/*
		 * Combine any new direct dispatch information from the subplan to
		 * the parent slice.
		 */
		MergeDirectDispatchCalculationInfo(&root->curSlice->directDispatch,
										   &sendSlice->directDispatch);

		return subplan;
	}

	switch (subpath->locus.locustype)
	{
		case CdbLocusType_Entry:
			/* cannot motion from Entry DB */
			sendSlice->gangType = GANGTYPE_ENTRYDB_READER;
			sendSlice->numsegments = 1;
			sendSlice->segindex = -1;
			break;

		case CdbLocusType_SingleQE:
			sendSlice->gangType = GANGTYPE_SINGLETON_READER;
			sendSlice->numsegments = 1;
			/*
			 * XXX: for now, always execute the slice in segment 0. Ideally, we
			 * would assign different SingleQEs to different segments to distribute
			 * the load more evenly, but keep it simple for now.
			 */
			sendSlice->segindex = 0;
			break;

		case CdbLocusType_General:
			/*  */
			sendSlice->gangType = GANGTYPE_PRIMARY_READER;
			sendSlice->numsegments = 1;
			sendSlice->segindex = 0;
			break;

		case CdbLocusType_SegmentGeneral:
			sendSlice->gangType = GANGTYPE_SINGLETON_READER;
			sendSlice->numsegments = subpath->locus.numsegments;
			sendSlice->segindex = 0;
			break;

		case CdbLocusType_Replicated:
			// is probably writer, set already
			//sendSlice->gangType == GANGTYPE_PRIMARY_READER;
			sendSlice->numsegments = subpath->locus.numsegments;
			sendSlice->segindex = 0;
			break;

		case CdbLocusType_OuterQuery:
			elog(ERROR, "unexpected Motion requested from OuterQuery locus");
			break;

		case CdbLocusType_Hashed:
		case CdbLocusType_HashedOJ:
		case CdbLocusType_Strewn:
			// might be writer, set already
			//sendSlice->gangType == GANGTYPE_PRIMARY_READER;
			sendSlice->numsegments = subpath->locus.numsegments;
			sendSlice->segindex = 0;
			break;

		default:
			elog(ERROR, "unknown locus type %d", subpath->locus.locustype);
	}

	/* Add motion operator. */
	motion = cdbpathtoplan_create_motion_plan(root, path, subplan);
	motion->senderSliceInfo = sendSlice;

	if (subpath->locus.locustype == CdbLocusType_Replicated)
		motion->motionType = MOTIONTYPE_GATHER_SINGLE;

	/* The topmost Plan in the sender slice must have 'flow' set correctly. */
	motion->plan.lefttree->flow = cdbpathtoplan_create_flow(root, subpath->locus);

	copy_generic_path_info(&motion->plan, (Path *) path);

	root->curOuterRels = save_curOuterRels;
	root->curOuterParams = save_curOuterParams;

	/*
	 * It's currently not allowed to direct-dispatch a slice that has a
	 * Motion that sends tuples to it. It would be possible in principle,
	 * but the interconnect initialization code gets confused. Give the
	 * direct dispatch machinery a chance to react to this Motion.
	 */
	if (Gp_role == GP_ROLE_DISPATCH && root->config->gp_enable_direct_dispatch)
		DirectDispatchUpdateContentIdsFromPlan(root, (Plan *) motion);

	return (Plan *) motion;
}	/* create_motion_plan */

/*
 * create_splitupdate_plan
 */
static Plan *
create_splitupdate_plan(PlannerInfo *root, SplitUpdatePath *path)
{
	Path	   *subpath = path->subpath;
	Plan	   *subplan;
	SplitUpdate *splitupdate;
	Relation	resultRel;
	TupleDesc	resultDesc;
	GpPolicy   *cdbpolicy;
	int			attrIdx;
	ListCell   *lc;
	int			lastresno;
	Oid		   *hashFuncs;
	int			i;

	resultRel = relation_open(planner_rt_fetch(path->resultRelation, root)->relid, NoLock);
	resultDesc = RelationGetDescr(resultRel);
	cdbpolicy = resultRel->rd_cdbpolicy;

	subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

	/* Transfer resname/resjunk labeling, too, to keep executor happy */
	apply_tlist_labeling(subplan->targetlist, root->processed_tlist);

	splitupdate = makeNode(SplitUpdate);

	splitupdate->plan.targetlist = NIL; /* filled in below */
	splitupdate->plan.qual = NIL;
	splitupdate->plan.lefttree = subplan;
	splitupdate->plan.righttree = NULL;

	copy_generic_path_info(&splitupdate->plan, (Path *) path);

	/*
	 * Build the insertColIdx and deleteColIdx arrays, to indicate how the
	 * inputs are mapped to the output tuples, for the DELETE and INSERT
	 * actions.
	 *
	 * For the DELETE rows, we only need the 'gp_segment_id' and 'ctid'
	 * junk columns, so we fill deleteColIdx with -1. The gp_segment_id
	 * column is used to indicate the target segment. In other words,
	 * there should be an Explicit Motion on top of the Split Update node.
	 * NOTE: ORCA uses SplitUpdate differently. It puts a Redistribute
	 * Motion on top of the SplitUpdate, and fills in the distribution key
	 * columns on DELETE rows with the old values. The Redistribute Motion
	 * then computes the target segment. So deleteColIdx is needed for
	 * ORCA, but we don't use it here.
	 */
	lc = list_head(subplan->targetlist);
	for (attrIdx = 1; attrIdx <= resultDesc->natts; ++attrIdx)
	{
		TargetEntry			*tle;
		Form_pg_attribute	attr;

		tle = (TargetEntry *) lfirst(lc);
		lc = lnext(lc);
		Assert(tle);

		attr = resultDesc->attrs[attrIdx - 1];
		if (attr->attisdropped)
		{
			Assert(IsA(tle->expr, Const) && ((Const *) tle->expr)->constisnull);
		}
		else
		{
			Assert(exprType((Node *) tle->expr) == attr->atttypid);
		}

		splitupdate->insertColIdx = lappend_int(splitupdate->insertColIdx, attrIdx);
		splitupdate->deleteColIdx = lappend_int(splitupdate->deleteColIdx, -1);

		splitupdate->plan.targetlist = lappend(splitupdate->plan.targetlist, tle);
	}
	lastresno = list_length(splitupdate->plan.targetlist);

	/* Copy all junk attributes. */
	for (; lc != NULL; lc = lnext(lc))
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		TargetEntry *newtle;

		if (!tle->resjunk)
			continue;

		newtle = makeTargetEntry(tle->expr,
								 ++lastresno,
								 tle->resname,
								 tle->resjunk);
		splitupdate->plan.targetlist = lappend(splitupdate->plan.targetlist, newtle);
	}
	splitupdate->plan.targetlist = lappend(splitupdate->plan.targetlist,
										   makeTargetEntry((Expr *) makeNode(DMLActionExpr),
														   ++lastresno,
														   "DMLAction",
														   true));

	/* Look up the right hash functions for the hash expressions */
	hashFuncs = palloc(cdbpolicy->nattrs * sizeof(Oid));
	for (i = 0; i < cdbpolicy->nattrs; i++)
	{
		AttrNumber	attnum = cdbpolicy->attrs[i];
		Oid			typeoid = resultDesc->attrs[attnum - 1]->atttypid;
		Oid			opfamily;

		opfamily = get_opclass_family(cdbpolicy->opclasses[i]);

		hashFuncs[i] = cdb_hashproc_in_opfamily(opfamily, typeoid);
	}
	splitupdate->numHashAttrs = cdbpolicy->nattrs;
	splitupdate->hashAttnos = palloc(cdbpolicy->nattrs * sizeof(AttrNumber));
	memcpy(splitupdate->hashAttnos, cdbpolicy->attrs, cdbpolicy->nattrs * sizeof(AttrNumber));
	splitupdate->hashFuncs = hashFuncs;
	splitupdate->numHashSegments = cdbpolicy->numsegments;

	relation_close(resultRel, NoLock);

	/*
	 * A SplitUpdate also computes the target segment ID, based on other columns,
	 * so we treat it the same as a Motion node for this purpose.
	 */
	root->numMotions++;

	return (Plan *) splitupdate;
}


/*****************************************************************************
 *
 *	BASE-RELATION SCAN METHODS
 *
 *****************************************************************************/


/*
 * create_seqscan_plan
 *	 Returns a seqscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SeqScan *
create_seqscan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses)
{
	SeqScan    *scan_plan;
	Index		scan_relid = best_path->parent->relid;

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->parent->rtekind == RTE_RELATION);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_seqscan(tlist,
							 scan_clauses,
							 scan_relid);

	copy_generic_path_info(&scan_plan->plan, best_path);

	return scan_plan;
}

/*
 * create_externalscan_plan
 *	 Returns an externalscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 *
 *	 The external plan also includes the data format specification and file
 *	 location specification. Here is where we do the mapping of external file
 *	 to segment database and add it to the plan (or bail out of the mapping
 *	 rules are broken)
 *
 *	 Mapping rules
 *	 -------------
 *	 - 'file' protocol: each location (URI of local file) gets mapped to one
 *						and one only primary segdb.
 *	 - 'http' protocol: each location (URI of http server) gets mapped to one
 *						and one only primary segdb.
 *	 - 'gpfdist' and 'gpfdists' protocols: all locations (URI of gpfdist(s) client) are mapped
 *						to all primary segdbs. If there are less URIs than
 *						segdbs (usually the case) the URIs are duplicated
 *						so that there will be one for each segdb. However, if
 *						the GUC variable gp_external_max_segs is set to a num
 *						less than (total segdbs/total URIs) then we make sure
 *						that no URI gets mapped to more than this GUC number by
 *						skipping some segdbs randomly.
 *	 - 'exec' protocol: all segdbs get mapped to execute the command (this is
 *						soon to be changed though).
 */
static ExternalScan *
create_externalscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses)
{
	ExternalScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RelOptInfo *rel = best_path->parent;
	List	   *filenames;
	bool		ismasteronly = false;
	bool		islimitinrows = false;
	int			rejectlimit = -1;
	bool		logerrors = false;
	ExtTableEntry *ext = rel->extEntry;

	/* it should be an external rel... */
	Assert(scan_relid > 0);
	Assert(rel->rtekind == RTE_RELATION);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	Assert(ext->execlocations != NIL);

	if (ext->rejectlimit != -1)
	{
		/*
		 * single row error handling is requested, make sure reject limit and
		 * error table (if requested) are valid.
		 *
		 * NOTE: this should never happen unless somebody modified the catalog
		 * manually. We are just being pedantic here.
		 */
		VerifyRejectLimit(ext->rejectlimittype, ext->rejectlimit);
	}

	/* assign Uris to segments. */
	filenames = create_external_scan_uri_list(ext, &ismasteronly);

	/* data format description */
	Assert(ext->fmtopts);

	/* single row error handling */
	if (ext->rejectlimit != -1)
	{
		islimitinrows = (ext->rejectlimittype == 'r' ? true : false);
		rejectlimit = ext->rejectlimit;
		logerrors = ext->logerrors;
	}

	scan_plan = make_externalscan(tlist,
								  scan_clauses,
								  scan_relid,
								  filenames,
								  ext->fmtopts,
								  ext->fmtcode,
								  ismasteronly,
								  rejectlimit,
								  islimitinrows,
								  logerrors,
								  ext->encoding);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_samplescan_plan
 *	 Returns a samplescan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SampleScan *
create_samplescan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	SampleScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	TableSampleClause *tsc;

	/* it should be a base rel with a tablesample clause... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_RELATION);
	tsc = rte->tablesample;
	Assert(tsc != NULL);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		tsc = (TableSampleClause *)
			replace_nestloop_params(root, (Node *) tsc);
	}

	scan_plan = make_samplescan(tlist,
								scan_clauses,
								scan_relid,
								tsc);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

List *
create_external_scan_uri_list(ExtTableEntry *ext, bool *ismasteronly)
{
	ListCell   *c;
	List	   *modifiedloclist = NIL;
	int			i;
	CdbComponentDatabases *db_info;
	int			total_primaries;
	char	  **segdb_file_map;

	/* various processing flags */
	bool		using_execute = false;	/* true if EXECUTE is used */
	bool		using_location; /* true if LOCATION is used */
	bool		found_candidate = false;
	bool		found_match = false;
	bool		done = false;
	List	   *filenames;

	/* gpfdist(s) or EXECUTE specific variables */
	int			total_to_skip = 0;
	int			max_participants_allowed = 0;
	int			num_segs_participating = 0;
	bool	   *skip_map = NULL;
	bool		should_skip_randomly = false;

	Uri		   *uri;
	char	   *on_clause;

	*ismasteronly = false;

	/* is this an EXECUTE table or a LOCATION (URI) table */
	if (ext->command)
	{
		using_execute = true;
		using_location = false;
	}
	else
	{
		using_execute = false;
		using_location = true;
	}

	/* is this an EXECUTE table or a LOCATION (URI) table */
	if (ext->command && !gp_external_enable_exec)
	{
		ereport(ERROR,
				(errcode(ERRCODE_GP_FEATURE_NOT_CONFIGURED),	/* any better errcode? */
				 errmsg("using external tables with OS level commands (EXECUTE clause) is disabled"),
				 errhint("To enable set gp_external_enable_exec=on.")));
	}

	/* various validations */
	if (ext->iswritable)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot read from a WRITABLE external table"),
				 errhint("Create the table as READABLE instead.")));

	/*
	 * take a peek at the first URI so we know which protocol we'll deal with
	 */
	if (!using_execute)
	{
		char	   *first_uri_str;

		first_uri_str = strVal(linitial(ext->urilocations));
		uri = ParseExternalTableUri(first_uri_str);
	}
	else
		uri = NULL;

	/* get the ON clause information, and restrict 'ON MASTER' to custom
	 * protocols only */
	on_clause = (char *) strVal(linitial(ext->execlocations));
	if ((strcmp(on_clause, "MASTER_ONLY") == 0)
		&& using_location && (uri->protocol != URI_CUSTOM)) {
		ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				errmsg("\'ON MASTER\' is not supported by this protocol yet")));
	}

	/* get the total valid primary segdb count */
	db_info = cdbcomponent_getCdbComponents();
	total_primaries = 0;
	for (i = 0; i < db_info->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *p = &db_info->segment_db_info[i];

		if (SEGMENT_IS_ACTIVE_PRIMARY(p))
			total_primaries++;
	}

	/*
	 * initialize a file-to-segdb mapping. segdb_file_map string array indexes
	 * segindex and the entries are the external file path is assigned to this
	 * segment database. For example if segdb_file_map[2] has "/tmp/emp.1" then
	 * this file is assigned to primary segdb 2. if an entry has NULL then
	 * that segdb isn't assigned any file.
	 */
	segdb_file_map = (char **) palloc0(total_primaries * sizeof(char *));

	/*
	 * Now we do the actual assignment of work to the segment databases (where
	 * work is either a URI to open or a command to execute). Due to the big
	 * differences between the different protocols we handle each one
	 * separately. Unfortunately this means some code duplication, but keeping
	 * this separation makes the code much more understandable and (even) more
	 * maintainable.
	 *
	 * Outline of the following code blocks (from simplest to most complex):
	 * (only one of these will get executed for a statement)
	 *
	 * 1) segment mapping for tables with LOCATION http:// or file:// .
	 *
	 * These two protocols are very similar in that they enforce a
	 * 1-URI:1-segdb relationship. The only difference between them is that
	 * file:// URI must be assigned to a segdb on a host that is local to that
	 * URI.
	 *
	 * 2) segment mapping for tables with LOCATION gpfdist(s):// or custom
	 * protocol
	 *
	 * This protocol is more complicated - in here we usually duplicate the
	 * user supplied gpfdist(s):// URIs until there is one available to every
	 * segdb. However, in some cases (as determined by gp_external_max_segs
	 * GUC) we don't want to use *all* segdbs but instead figure out how many
	 * and pick them randomly (this is mainly for better performance and
	 * resource mgmt).
	 *
	 * 3) segment mapping for tables with EXECUTE 'cmd' ON.
	 *
	 * In here we don't have URI's. We have a single command string and a
	 * specification of the segdb granularity it should get executed on (the
	 * ON clause). Depending on the ON clause specification we could go many
	 * different ways, for example: assign the command to all segdb, or one
	 * command per host, or assign to 5 random segments, etc...
	 */

	/* (1) */
	if (using_location && (uri->protocol == URI_FILE || uri->protocol == URI_HTTP))
	{
		/*
		 * extract file path and name from URI strings and assign them a
		 * primary segdb
		 */
		foreach(c, ext->urilocations)
		{
			const char *uri_str = (char *) strVal(lfirst(c));

			uri = ParseExternalTableUri(uri_str);

			found_candidate = false;
			found_match = false;

			/*
			 * look through our segment database list and try to find a
			 * database that can handle this uri.
			 */
			for (i = 0; i < db_info->total_segment_dbs && !found_match; i++)
			{
				CdbComponentDatabaseInfo *p = &db_info->segment_db_info[i];
				int segind = p->config->segindex;

				/*
				 * Assign mapping of external file to this segdb only if:
				 * 1) This segdb is a valid primary.
				 * 2) An external file wasn't already assigned to it.
				 * 3) If 'file' protocol, host of segdb and file must be
				 *    the same.
				 *
				 * This logic also guarantees that file that appears first in
				 * the external location list for the same host gets assigned
				 * the segdb with the lowest index for this host.
				 */
				if (SEGMENT_IS_ACTIVE_PRIMARY(p))
				{
					if (uri->protocol == URI_FILE)
					{
						if (pg_strcasecmp(uri->hostname, p->config->hostname) != 0 && pg_strcasecmp(uri->hostname, p->config->address) != 0)
							continue;
					}

					/* a valid primary segdb exist on this host */
					found_candidate = true;

					if (segdb_file_map[segind] == NULL)
					{
						/* segdb not taken yet. assign this URI to this segdb */
						segdb_file_map[segind] = pstrdup(uri_str);
						found_match = true;
					}

					/*
					 * too bad. this segdb already has an external source
					 * assigned
					 */
				}
			}

			/*
			 * We failed to find a segdb for this URI.
			 */
			if (!found_match)
			{
				if (uri->protocol == URI_FILE)
				{
					if (found_candidate)
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("could not assign a segment database for \"%s\"",
										uri_str),
								 errdetail("There are more external files than primary segment databases on host \"%s\"",
										   uri->hostname)));
					}
					else
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("could not assign a segment database for \"%s\"",
										uri_str),
								 errdetail("There isn't a valid primary segment database on host \"%s\"",
										   uri->hostname)));
					}
				}
				else	/* HTTP */
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("could not assign a segment database for \"%s\"",
									uri_str),
							 errdetail("There are more URIs than total primary segment databases")));
				}
			}
		}


	}
	/* (2) */
	else if (using_location && (uri->protocol == URI_GPFDIST ||
							   uri->protocol == URI_GPFDISTS ||
							   uri->protocol == URI_CUSTOM))
	{
		if ((strcmp(on_clause, "MASTER_ONLY") == 0) && (uri->protocol == URI_CUSTOM))
		{
			const char *uri_str = strVal(linitial(ext->urilocations));
			segdb_file_map[0] = pstrdup(uri_str);
			*ismasteronly = true;
		}
		else
		{
			/*
			 * Re-write the location list for GPFDIST or GPFDISTS before mapping to segments.
			 *
			 * If we happen to be dealing with URI's with the 'gpfdist' (or 'gpfdists') protocol
			 * we do an extra step here.
			 *
			 * (*) We modify the urilocationlist so that every
			 * primary segdb will get a URI (therefore we duplicate the existing
			 * URI's until the list is of size = total_primaries).
			 * Example: 2 URIs, 7 total segdbs.
			 * Original LocationList: URI1->URI2
			 * Modified LocationList: URI1->URI2->URI1->URI2->URI1->URI2->URI1
			 *
			 * (**) We also make sure that we don't allocate more segdbs than
			 * (# of URIs x gp_external_max_segs).
			 * Example: 2 URIs, 7 total segdbs, gp_external_max_segs = 3
			 * Original LocationList: URI1->URI2
			 * Modified LocationList: URI1->URI2->URI1->URI2->URI1->URI2 (6 total).
			 *
			 * (***) In that case that we need to allocate only a subset of primary
			 * segdbs and not all we then also create a random map of segments to skip.
			 * Using the previous example a we create a map of 7 entries and need to
			 * randomly select 1 segdb to skip (7 - 6 = 1). so it may look like this:
			 * [F F T F F F F] - in which case we know to skip the 3rd segment only.
			 */

			/* total num of segs that will participate in the external operation */
			num_segs_participating = total_primaries;

			/* max num of segs that are allowed to participate in the operation */
			if ((uri->protocol == URI_GPFDIST) || (uri->protocol == URI_GPFDISTS))
			{
				max_participants_allowed = list_length(ext->urilocations) *
					gp_external_max_segs;
			}
			else
			{
				/*
				 * for custom protocol, set max_participants_allowed to
				 * num_segs_participating so that assignment to segments will use
				 * all available segments
				 */
				max_participants_allowed = num_segs_participating;
			}

			elog(DEBUG5,
				 "num_segs_participating = %d. max_participants_allowed = %d. number of URIs = %d",
				 num_segs_participating, max_participants_allowed, list_length(ext->urilocations));

			/* see (**) above */
			if (num_segs_participating > max_participants_allowed)
			{
				total_to_skip = num_segs_participating - max_participants_allowed;
				num_segs_participating = max_participants_allowed;
				should_skip_randomly = true;

				elog(NOTICE, "External scan %s will utilize %d out "
					 "of %d segment databases",
					 (uri->protocol == URI_GPFDIST ? "from gpfdist(s) server" : "using custom protocol"),
					 num_segs_participating,
					 total_primaries);
			}

			if (list_length(ext->urilocations) > num_segs_participating)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("there are more external files (URLs) than primary segments that can read them"),
						 errdetail("Found %d URLs and %d primary segments.",
								   list_length(ext->urilocations),
								   num_segs_participating)));

			/*
			 * restart location list and fill in new list until number of
			 * locations equals the number of segments participating in this
			 * action (see (*) above for more details).
			 */
			while (!done)
			{
				foreach(c, ext->urilocations)
				{
					char	   *uri_str = (char *) strVal(lfirst(c));

					/* append to a list of Value nodes, size nelems */
					modifiedloclist = lappend(modifiedloclist, makeString(pstrdup(uri_str)));

					if (list_length(modifiedloclist) == num_segs_participating)
					{
						done = true;
						break;
					}

					if (list_length(modifiedloclist) > num_segs_participating)
					{
						elog(ERROR, "External scan location list failed building distribution.");
					}
				}
			}

			/* See (***) above for details */
			if (should_skip_randomly)
				skip_map = makeRandomSegMap(total_primaries, total_to_skip);

			/*
			 * assign each URI from the new location list a primary segdb
			 */
			foreach(c, modifiedloclist)
			{
				const char *uri_str = strVal(lfirst(c));

				uri = ParseExternalTableUri(uri_str);

				found_candidate = false;
				found_match = false;

				/*
				 * look through our segment database list and try to find a
				 * database that can handle this uri.
				 */
				for (i = 0; i < db_info->total_segment_dbs && !found_match; i++)
				{
					CdbComponentDatabaseInfo *p = &db_info->segment_db_info[i];
					int			segind = p->config->segindex;

					/*
					 * Assign mapping of external file to this segdb only if:
					 * 1) This segdb is a valid primary.
					 * 2) An external file wasn't already assigned to it.
					 */
					if (SEGMENT_IS_ACTIVE_PRIMARY(p))
					{
						/*
						 * skip this segdb if skip_map for this seg index tells us
						 * to skip it (set to 'true').
						 */
						if (should_skip_randomly)
						{
							Assert(segind < total_primaries);

							if (skip_map[segind])
								continue;	/* skip it */
						}

						/* a valid primary segdb exist on this host */
						found_candidate = true;

						if (segdb_file_map[segind] == NULL)
						{
							/* segdb not taken yet. assign this URI to this segdb */
							segdb_file_map[segind] = pstrdup(uri_str);
							found_match = true;
						}

						/*
						 * too bad. this segdb already has an external source
						 * assigned
						 */
					}
				}

				/* We failed to find a segdb for this gpfdist(s) URI */
				if (!found_match)
				{
					/* should never happen */
					elog(LOG,
						 "external tables gpfdist(s) allocation error. "
						 "total_primaries: %d, num_segs_participating %d "
						 "max_participants_allowed %d, total_to_skip %d",
						 total_primaries, num_segs_participating,
						 max_participants_allowed, total_to_skip);

					elog(ERROR,
						 "internal error in createplan for external tables when trying to assign segments for gpfdist(s)");
				}
			}
		}
	}
	/* (3) */
	else if (using_execute)
	{
		const char *command = ext->command;
		const char *prefix = "execute:";
		char	   *prefixed_command;

		/* build the command string for the executor - 'execute:command' */
		StringInfo	buf = makeStringInfo();

		appendStringInfo(buf, "%s%s", prefix, command);
		prefixed_command = pstrdup(buf->data);

		pfree(buf->data);
		pfree(buf);
		buf = NULL;

		/*
		 * Now we handle each one of the ON locations separately:
		 *
		 * 1) all segs
		 * 2) one per host
		 * 3) all segs on host <foo>
		 * 4) seg <n> only
		 * 5) <n> random segs
		 * 6) master only
		 */
		if (strcmp(on_clause, "ALL_SEGMENTS") == 0)
		{
			/* all segments get a copy of the command to execute */

			for (i = 0; i < db_info->total_segment_dbs; i++)
			{
				CdbComponentDatabaseInfo *p = &db_info->segment_db_info[i];
				int			segind = p->config->segindex;

				if (SEGMENT_IS_ACTIVE_PRIMARY(p))
					segdb_file_map[segind] = pstrdup(prefixed_command);
			}

		}
		else if (strcmp(on_clause, "PER_HOST") == 0)
		{
			/* 1 seg per host */

			List	   *visited_hosts = NIL;
			ListCell   *lc;

			for (i = 0; i < db_info->total_segment_dbs; i++)
			{
				CdbComponentDatabaseInfo *p = &db_info->segment_db_info[i];
				int			segind = p->config->segindex;

				if (SEGMENT_IS_ACTIVE_PRIMARY(p))
				{
					bool		host_taken = false;

					foreach(lc, visited_hosts)
					{
						const char *hostname = strVal(lfirst(lc));

						if (pg_strcasecmp(hostname, p->config->hostname) == 0)
						{
							host_taken = true;
							break;
						}
					}

					/*
					 * if not assigned to a seg on this host before - do it
					 * now and add this hostname to the list so that we don't
					 * use segs on this host again.
					 */
					if (!host_taken)
					{
						segdb_file_map[segind] = pstrdup(prefixed_command);
						visited_hosts = lappend(visited_hosts,
										   makeString(pstrdup(p->config->hostname)));
					}
				}
			}
		}
		else if (strncmp(on_clause, "HOST:", strlen("HOST:")) == 0)
		{
			/* all segs on the specified host get copy of the command */
			char	   *hostname = on_clause + strlen("HOST:");
			bool		match_found = false;

			for (i = 0; i < db_info->total_segment_dbs; i++)
			{
				CdbComponentDatabaseInfo *p = &db_info->segment_db_info[i];
				int			segind = p->config->segindex;

				if (SEGMENT_IS_ACTIVE_PRIMARY(p) &&
					pg_strcasecmp(hostname, p->config->hostname) == 0)
				{
					segdb_file_map[segind] = pstrdup(prefixed_command);
					match_found = true;
				}
			}

			if (!match_found)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("could not assign a segment database for command \"%s\")",
								command),
						 errdetail("No valid primary segment was found in the requested host name \"%s\".",
								hostname)));
		}
		else if (strncmp(on_clause, "SEGMENT_ID:", strlen("SEGMENT_ID:")) == 0)
		{
			/* 1 seg with specified id gets a copy of the command */
			int			target_segid = atoi(on_clause + strlen("SEGMENT_ID:"));
			bool		match_found = false;

			for (i = 0; i < db_info->total_segment_dbs; i++)
			{
				CdbComponentDatabaseInfo *p = &db_info->segment_db_info[i];
				int			segind = p->config->segindex;

				if (SEGMENT_IS_ACTIVE_PRIMARY(p) && segind == target_segid)
				{
					segdb_file_map[segind] = pstrdup(prefixed_command);
					match_found = true;
				}
			}

			if (!match_found)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("could not assign a segment database for command \"%s\"",
								command),
						 errdetail("The requested segment id %d is not a valid primary segment or doesn't exist in the database",
								   target_segid)));
		}
		else if (strncmp(on_clause, "TOTAL_SEGS:", strlen("TOTAL_SEGS:")) == 0)
		{
			/* total n segments selected randomly */

			int			num_segs_to_use = atoi(on_clause + strlen("TOTAL_SEGS:"));

			if (num_segs_to_use > total_primaries)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("table defined with EXECUTE ON %d but there are only %d valid primary segments in the database",
								num_segs_to_use, total_primaries)));

			total_to_skip = total_primaries - num_segs_to_use;
			skip_map = makeRandomSegMap(total_primaries, total_to_skip);

			for (i = 0; i < db_info->total_segment_dbs; i++)
			{
				CdbComponentDatabaseInfo *p = &db_info->segment_db_info[i];
				int			segind = p->config->segindex;

				if (SEGMENT_IS_ACTIVE_PRIMARY(p))
				{
					Assert(segind < total_primaries);
					if (skip_map[segind])
						continue;		/* skip it */

					segdb_file_map[segind] = pstrdup(prefixed_command);
				}
			}
		}
		else if (strcmp(on_clause, "MASTER_ONLY") == 0)
		{
			/*
			 * store the command in first array entry and indicate that it is
			 * meant for the master segment (not seg o).
			 */
			segdb_file_map[0] = pstrdup(prefixed_command);
			*ismasteronly = true;
		}
		else
		{
			elog(ERROR, "Internal error in createplan for external tables: got invalid ON clause code %s",
				 on_clause);
		}
	}
	else
	{
		/* should never get here */
		elog(ERROR, "Internal error in createplan for external tables");
	}

	/*
	 * convert array map to a list so it can be serialized as part of the plan
	 */
	filenames = NIL;
	for (i = 0; i < total_primaries; i++)
	{
		if (segdb_file_map[i] != NULL)
			filenames = lappend(filenames, makeString(segdb_file_map[i]));
		else
		{
			/* no file for this segdb. add a null entry */
			Value	   *n = makeNode(Value);

			n->type = T_Null;
			filenames = lappend(filenames, n);
		}
	}

	return filenames;
}


/*
 * create_indexscan_plan
 *	  Returns an indexscan plan for the base relation scanned by 'best_path'
 *	  with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 *
 * We use this for both plain IndexScans and IndexOnlyScans, because the
 * qual preprocessing work is the same for both.  Note that the caller tells
 * us which to build --- we don't look at best_path->path.pathtype, because
 * create_bitmap_subplan needs to be able to override the prior decision.
 */
static Scan *
create_indexscan_plan(PlannerInfo *root,
					  IndexPath *best_path,
					  List *tlist,
					  List *scan_clauses,
					  bool indexonly)
{
	Scan	   *scan_plan;
	List	   *indexquals = best_path->indexquals;
	List	   *indexorderbys = best_path->indexorderbys;
	Index		baserelid = best_path->path.parent->relid;
	Oid			indexoid = best_path->indexinfo->indexoid;
	List	   *qpqual;
	List	   *stripped_indexquals;
	List	   *fixed_indexquals;
	List	   *fixed_indexorderbys;
	List	   *indexorderbyops = NIL;
	ListCell   *l;

	/* it should be a base rel... */
	Assert(baserelid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/*
	 * Build "stripped" indexquals structure (no RestrictInfos) to pass to
	 * executor as indexqualorig
	 */
	stripped_indexquals = get_actual_clauses(indexquals);

	/*
	 * The executor needs a copy with the indexkey on the left of each clause
	 * and with index Vars substituted for table ones.
	 */
	fixed_indexquals = fix_indexqual_references(root, best_path);

	/*
	 * Likewise fix up index attr references in the ORDER BY expressions.
	 */
	fixed_indexorderbys = fix_indexorderby_references(root, best_path);

	/*
	 * The qpqual list must contain all restrictions not automatically handled
	 * by the index, other than pseudoconstant clauses which will be handled
	 * by a separate gating plan node.  All the predicates in the indexquals
	 * will be checked (either by the index itself, or by nodeIndexscan.c),
	 * but if there are any "special" operators involved then they must be
	 * included in qpqual.  The upshot is that qpqual must contain
	 * scan_clauses minus whatever appears in indexquals.
	 *
	 * In normal cases simple pointer equality checks will be enough to spot
	 * duplicate RestrictInfos, so we try that first.
	 *
	 * Another common case is that a scan_clauses entry is generated from the
	 * same EquivalenceClass as some indexqual, and is therefore redundant
	 * with it, though not equal.  (This happens when indxpath.c prefers a
	 * different derived equality than what generate_join_implied_equalities
	 * picked for a parameterized scan's ppi_clauses.)
	 *
	 * In some situations (particularly with OR'd index conditions) we may
	 * have scan_clauses that are not equal to, but are logically implied by,
	 * the index quals; so we also try a predicate_implied_by() check to see
	 * if we can discard quals that way.  (predicate_implied_by assumes its
	 * first input contains only immutable functions, so we have to check
	 * that.)
	 *
	 * Note: if you change this bit of code you should also look at
	 * extract_nonindex_conditions() in costsize.c.
	 */
	qpqual = NIL;
	foreach(l, scan_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		Assert(IsA(rinfo, RestrictInfo));
		if (rinfo->pseudoconstant)
			continue;			/* we may drop pseudoconstants here */
		if (list_member_ptr(indexquals, rinfo))
			continue;			/* simple duplicate */
		if (is_redundant_derived_clause(rinfo, indexquals))
			continue;			/* derived from same EquivalenceClass */
		if (!contain_mutable_functions((Node *) rinfo->clause) &&
			predicate_implied_by(list_make1(rinfo->clause), indexquals))
			continue;			/* provably implied by indexquals */
		qpqual = lappend(qpqual, rinfo);
	}

	/* Sort clauses into best execution order */
	qpqual = order_qual_clauses(root, qpqual);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	qpqual = extract_actual_clauses(qpqual, false);

	/*
	 * We have to replace any outer-relation variables with nestloop params in
	 * the indexqualorig, qpqual, and indexorderbyorig expressions.  A bit
	 * annoying to have to do this separately from the processing in
	 * fix_indexqual_references --- rethink this when generalizing the inner
	 * indexscan support.  But note we can't really do this earlier because
	 * it'd break the comparisons to predicates above ... (or would it?  Those
	 * wouldn't have outer refs)
	 */
	if (best_path->path.param_info)
	{
		stripped_indexquals = (List *)
			replace_nestloop_params(root, (Node *) stripped_indexquals);
		qpqual = (List *)
			replace_nestloop_params(root, (Node *) qpqual);
		indexorderbys = (List *)
			replace_nestloop_params(root, (Node *) indexorderbys);
	}

	/*
	 * If there are ORDER BY expressions, look up the sort operators for their
	 * result datatypes.
	 */
	if (indexorderbys)
	{
		ListCell   *pathkeyCell,
				   *exprCell;

		/*
		 * PathKey contains OID of the btree opfamily we're sorting by, but
		 * that's not quite enough because we need the expression's datatype
		 * to look up the sort operator in the operator family.
		 */
		Assert(list_length(best_path->path.pathkeys) == list_length(indexorderbys));
		forboth(pathkeyCell, best_path->path.pathkeys, exprCell, indexorderbys)
		{
			PathKey    *pathkey = (PathKey *) lfirst(pathkeyCell);
			Node	   *expr = (Node *) lfirst(exprCell);
			Oid			exprtype = exprType(expr);
			Oid			sortop;

			/* Get sort operator from opfamily */
			sortop = get_opfamily_member(pathkey->pk_opfamily,
										 exprtype,
										 exprtype,
										 pathkey->pk_strategy);
			if (!OidIsValid(sortop))
				elog(ERROR, "failed to find sort operator for ORDER BY expression");
			indexorderbyops = lappend_oid(indexorderbyops, sortop);
		}
	}

	/* Finally ready to build the plan node */
	if (indexonly)
		scan_plan = (Scan *) make_indexonlyscan(tlist,
												qpqual,
												baserelid,
												indexoid,
												fixed_indexquals,
												stripped_indexquals,
												fixed_indexorderbys,
												best_path->indexinfo->indextlist,
												best_path->indexscandir);
	else
		scan_plan = (Scan *) make_indexscan(tlist,
											qpqual,
											baserelid,
											indexoid,
											fixed_indexquals,
											stripped_indexquals,
											fixed_indexorderbys,
											indexorderbys,
											indexorderbyops,
											best_path->indexscandir);

	copy_generic_path_info(&scan_plan->plan, &best_path->path);

	return scan_plan;
}

/*
 * create_bitmap_scan_plan
 *	  Returns a bitmap scan plan for the base relation scanned by 'best_path'
 *	  with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static BitmapHeapScan *
create_bitmap_scan_plan(PlannerInfo *root,
						BitmapHeapPath *best_path,
						List *tlist,
						List *scan_clauses)
{
	Index		baserelid = best_path->path.parent->relid;
	Plan	   *bitmapqualplan;
	List	   *bitmapqualorig;
	List	   *indexquals;
	List	   *indexECs;
	List	   *qpqual;
	ListCell   *l;
	BitmapHeapScan *scan_plan;

	/* it should be a base rel... */
	Assert(baserelid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/* Process the bitmapqual tree into a Plan tree and qual lists */
	bitmapqualplan = create_bitmap_subplan(root, best_path->bitmapqual,
										   &bitmapqualorig, &indexquals,
										   &indexECs);

	/*
	 * The qpqual list must contain all restrictions not automatically handled
	 * by the index, other than pseudoconstant clauses which will be handled
	 * by a separate gating plan node.  All the predicates in the indexquals
	 * will be checked (either by the index itself, or by
	 * nodeBitmapHeapscan.c), but if there are any "special" operators
	 * involved then they must be added to qpqual.  The upshot is that qpqual
	 * must contain scan_clauses minus whatever appears in indexquals.
	 *
	 * This loop is similar to the comparable code in create_indexscan_plan(),
	 * but with some differences because it has to compare the scan clauses to
	 * stripped (no RestrictInfos) indexquals.  See comments there for more
	 * info.
	 *
	 * In normal cases simple equal() checks will be enough to spot duplicate
	 * clauses, so we try that first.  We next see if the scan clause is
	 * redundant with any top-level indexqual by virtue of being generated
	 * from the same EC.  After that, try predicate_implied_by().
	 *
	 * Unlike create_indexscan_plan(), the predicate_implied_by() test here is
	 * useful for getting rid of qpquals that are implied by index predicates,
	 * because the predicate conditions are included in the "indexquals"
	 * returned by create_bitmap_subplan().  Bitmap scans have to do it that
	 * way because predicate conditions need to be rechecked if the scan
	 * becomes lossy, so they have to be included in bitmapqualorig.
	 */
	qpqual = NIL;
	foreach(l, scan_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
		Node	   *clause = (Node *) rinfo->clause;

		Assert(IsA(rinfo, RestrictInfo));
		if (rinfo->pseudoconstant)
			continue;			/* we may drop pseudoconstants here */
		if (list_member(indexquals, clause))
			continue;			/* simple duplicate */
		if (rinfo->parent_ec && list_member_ptr(indexECs, rinfo->parent_ec))
			continue;			/* derived from same EquivalenceClass */
		if (!contain_mutable_functions(clause) &&
			predicate_implied_by(list_make1(clause), indexquals))
			continue;			/* provably implied by indexquals */
		qpqual = lappend(qpqual, rinfo);
	}

	/* Sort clauses into best execution order */
	qpqual = order_qual_clauses(root, qpqual);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	qpqual = extract_actual_clauses(qpqual, false);

	/*
	 * When dealing with special operators, we will at this point have
	 * duplicate clauses in qpqual and bitmapqualorig.  We may as well drop
	 * 'em from bitmapqualorig, since there's no point in making the tests
	 * twice.
	 */
	bitmapqualorig = list_difference_ptr(bitmapqualorig, qpqual);

	/*
	 * We have to replace any outer-relation variables with nestloop params in
	 * the qpqual and bitmapqualorig expressions.  (This was already done for
	 * expressions attached to plan nodes in the bitmapqualplan tree.)
	 */
	if (best_path->path.param_info)
	{
		qpqual = (List *)
			replace_nestloop_params(root, (Node *) qpqual);
		bitmapqualorig = (List *)
			replace_nestloop_params(root, (Node *) bitmapqualorig);
	}

	/* Finally ready to build the plan node */
	scan_plan = make_bitmap_heapscan(tlist,
									 qpqual,
									 bitmapqualplan,
									 bitmapqualorig,
									 baserelid);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * Given a bitmapqual tree, generate the Plan tree that implements it
 *
 * As byproducts, we also return in *qual and *indexqual the qual lists
 * (in implicit-AND form, without RestrictInfos) describing the original index
 * conditions and the generated indexqual conditions.  (These are the same in
 * simple cases, but when special index operators are involved, the former
 * list includes the special conditions while the latter includes the actual
 * indexable conditions derived from them.)  Both lists include partial-index
 * predicates, because we have to recheck predicates as well as index
 * conditions if the bitmap scan becomes lossy.
 *
 * In addition, we return a list of EquivalenceClass pointers for all the
 * top-level indexquals that were possibly-redundantly derived from ECs.
 * This allows removal of scan_clauses that are redundant with such quals.
 * (We do not attempt to detect such redundancies for quals that are within
 * OR subtrees.  This could be done in a less hacky way if we returned the
 * indexquals in RestrictInfo form, but that would be slower and still pretty
 * messy, since we'd have to build new RestrictInfos in many cases.)
 */
static Plan *
create_bitmap_subplan(PlannerInfo *root, Path *bitmapqual,
					  List **qual, List **indexqual, List **indexECs)
{
	Plan	   *plan;

	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;
		List	   *subplans = NIL;
		List	   *subquals = NIL;
		List	   *subindexquals = NIL;
		List	   *subindexECs = NIL;
		ListCell   *l;

		/*
		 * There may well be redundant quals among the subplans, since a
		 * top-level WHERE qual might have gotten used to form several
		 * different index quals.  We don't try exceedingly hard to eliminate
		 * redundancies, but we do eliminate obvious duplicates by using
		 * list_concat_unique.
		 */
		foreach(l, apath->bitmapquals)
		{
			Plan	   *subplan;
			List	   *subqual;
			List	   *subindexqual;
			List	   *subindexEC;

			subplan = create_bitmap_subplan(root, (Path *) lfirst(l),
											&subqual, &subindexqual,
											&subindexEC);
			subplans = lappend(subplans, subplan);
			subquals = list_concat_unique(subquals, subqual);
			subindexquals = list_concat_unique(subindexquals, subindexqual);
			/* Duplicates in indexECs aren't worth getting rid of */
			subindexECs = list_concat(subindexECs, subindexEC);
		}
		plan = (Plan *) make_bitmap_and(subplans);
		plan->startup_cost = apath->path.startup_cost;
		plan->total_cost = apath->path.total_cost;
		plan->plan_rows =
			clamp_row_est(apath->bitmapselectivity * apath->path.parent->tuples);
		plan->plan_width = 0;	/* meaningless */
		plan->parallel_aware = false;
		*qual = subquals;
		*indexqual = subindexquals;
		*indexECs = subindexECs;
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;
		List	   *subplans = NIL;
		List	   *subquals = NIL;
		List	   *subindexquals = NIL;
		bool		const_true_subqual = false;
		bool		const_true_subindexqual = false;
		ListCell   *l;

		/*
		 * Here, we only detect qual-free subplans.  A qual-free subplan would
		 * cause us to generate "... OR true ..."  which we may as well reduce
		 * to just "true".  We do not try to eliminate redundant subclauses
		 * because (a) it's not as likely as in the AND case, and (b) we might
		 * well be working with hundreds or even thousands of OR conditions,
		 * perhaps from a long IN list.  The performance of list_append_unique
		 * would be unacceptable.
		 */
		foreach(l, opath->bitmapquals)
		{
			Plan	   *subplan;
			List	   *subqual;
			List	   *subindexqual;
			List	   *subindexEC;

			subplan = create_bitmap_subplan(root, (Path *) lfirst(l),
											&subqual, &subindexqual,
											&subindexEC);
			subplans = lappend(subplans, subplan);
			if (subqual == NIL)
				const_true_subqual = true;
			else if (!const_true_subqual)
				subquals = lappend(subquals,
								   make_ands_explicit(subqual));
			if (subindexqual == NIL)
				const_true_subindexqual = true;
			else if (!const_true_subindexqual)
				subindexquals = lappend(subindexquals,
										make_ands_explicit(subindexqual));
		}

		/*
		 * In the presence of ScalarArrayOpExpr quals, we might have built
		 * BitmapOrPaths with just one subpath; don't add an OR step.
		 */
		if (list_length(subplans) == 1)
		{
			plan = (Plan *) linitial(subplans);
		}
		else
		{
			plan = (Plan *) make_bitmap_or(subplans);
			plan->startup_cost = opath->path.startup_cost;
			plan->total_cost = opath->path.total_cost;
			plan->plan_rows =
				clamp_row_est(opath->bitmapselectivity * opath->path.parent->tuples);
			plan->plan_width = 0;		/* meaningless */
			plan->parallel_aware = false;
		}

		/*
		 * If there were constant-TRUE subquals, the OR reduces to constant
		 * TRUE.  Also, avoid generating one-element ORs, which could happen
		 * due to redundancy elimination or ScalarArrayOpExpr quals.
		 */
		if (const_true_subqual)
			*qual = NIL;
		else if (list_length(subquals) <= 1)
			*qual = subquals;
		else
			*qual = list_make1(make_orclause(subquals));
		if (const_true_subindexqual)
			*indexqual = NIL;
		else if (list_length(subindexquals) <= 1)
			*indexqual = subindexquals;
		else
			*indexqual = list_make1(make_orclause(subindexquals));
		*indexECs = NIL;
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;
		IndexScan  *iscan;
		List	   *subindexECs;
		ListCell   *l;

		/* Use the regular indexscan plan build machinery... */
		iscan = (IndexScan *) create_indexscan_plan(root, ipath,
													NIL, NIL, false);
		Assert(IsA(iscan, IndexScan));
		/* then convert to a bitmap indexscan */
		plan = (Plan *) make_bitmap_indexscan(iscan->scan.scanrelid,
											  iscan->indexid,
											  iscan->indexqual,
											  iscan->indexqualorig);
		/* and set its cost/width fields appropriately */
		plan->startup_cost = 0.0;
		plan->total_cost = ipath->indextotalcost;
		plan->plan_rows =
			clamp_row_est(ipath->indexselectivity * ipath->path.parent->tuples);
		plan->plan_width = 0;	/* meaningless */
		plan->parallel_aware = false;
		*qual = get_actual_clauses(ipath->indexclauses);
		*indexqual = get_actual_clauses(ipath->indexquals);
		foreach(l, ipath->indexinfo->indpred)
		{
			Expr	   *pred = (Expr *) lfirst(l);

			/*
			 * We know that the index predicate must have been implied by the
			 * query condition as a whole, but it may or may not be implied by
			 * the conditions that got pushed into the bitmapqual.  Avoid
			 * generating redundant conditions.
			 */
			if (!predicate_implied_by(list_make1(pred), ipath->indexclauses))
			{
				*qual = lappend(*qual, pred);
				*indexqual = lappend(*indexqual, pred);
			}
		}
		subindexECs = NIL;
		foreach(l, ipath->indexquals)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

			if (rinfo->parent_ec)
				subindexECs = lappend(subindexECs, rinfo->parent_ec);
		}
		*indexECs = subindexECs;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));
		plan = NULL;			/* keep compiler quiet */
	}

	if (Gp_role == GP_ROLE_DISPATCH && root->config->gp_enable_direct_dispatch)
		DirectDispatchUpdateContentIdsFromPlan(root, plan);

	return plan;
}

/*
 * create_tidscan_plan
 *	 Returns a tidscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static TidScan *
create_tidscan_plan(PlannerInfo *root, TidPath *best_path,
					List *tlist, List *scan_clauses)
{
	TidScan    *scan_plan;
	Index		scan_relid = best_path->path.parent->relid;
	List	   *tidquals = best_path->tidquals;
	List	   *ortidquals;

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		tidquals = (List *)
			replace_nestloop_params(root, (Node *) tidquals);
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	/*
	 * Remove any clauses that are TID quals.  This is a bit tricky since the
	 * tidquals list has implicit OR semantics.
	 *
	 * In the case of CURRENT OF, however, we do want the CurrentOfExpr to
	 * reside in both the tidlist and the qual, as CurrentOfExpr is effectively
	 * a ctid, gp_segment_id, and tableoid qual. Constant folding will
	 * finish up this qual rewriting to ensure what we dispatch is a sane interpretation
	 * of CURRENT OF behavior.
	 */
	if (!(list_length(scan_clauses) == 1 && IsA(linitial(scan_clauses), CurrentOfExpr)))
	{
		ortidquals = tidquals;
		if (list_length(ortidquals) > 1)
			ortidquals = list_make1(make_orclause(ortidquals));
		scan_clauses = list_difference(scan_clauses, ortidquals);
	}

	scan_plan = make_tidscan(tlist,
							 scan_clauses,
							 scan_relid,
							 tidquals);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_subqueryscan_plan
 *	 Returns a subqueryscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SubqueryScan *
create_subqueryscan_plan(PlannerInfo *root, SubqueryScanPath *best_path,
						 List *tlist, List *scan_clauses)
{
	SubqueryScan *scan_plan;
	RelOptInfo *rel = best_path->path.parent;
	Index		scan_relid = rel->relid;
	Plan	   *subplan;

	/* it should be a subquery base rel... */
	Assert(scan_relid > 0);
	Assert(rel->rtekind == RTE_SUBQUERY);

	/*
	 * Recursively create Plan from Path for subquery.  Since we are entering
	 * a different planner context (subroot), recurse to create_plan not
	 * create_plan_recurse.
	 */
	subplan = create_plan(rel->subroot, best_path->subpath, root->curSlice);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		process_subquery_nestloop_params(root,
										 rel->subplan_params);
	}

	scan_plan = make_subqueryscan(tlist,
								  scan_clauses,
								  scan_relid,
								  subplan);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_functionscan_plan
 *	 Returns a functionscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static FunctionScan *
create_functionscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses)
{
	FunctionScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	List	   *functions;

	/* it should be a function base rel... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_FUNCTION);
	functions = rte->functions;

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		/* The function expressions could contain nestloop params, too */
		functions = (List *) replace_nestloop_params(root, (Node *) functions);
	}

	scan_plan = make_functionscan(tlist, scan_clauses, scan_relid,
								  functions, rte->funcordinality);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_tablefunction_plan
 *	 Returns a TableFunction plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static TableFunctionScan *
create_tablefunction_plan(PlannerInfo *root,
						  TableFunctionScanPath *best_path,
						  List *tlist,
						  List *scan_clauses)
{
	TableFunctionScan *tablefunc;
	RelOptInfo *rel = best_path->path.parent;
	Plan	   *subplan;
	Index		scan_relid = rel->relid;
	RangeTblEntry *rte;
	RangeTblFunction *rtf;

	/* it should be a function base rel... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rel->rtekind == RTE_TABLEFUNCTION);
	Assert(list_length(rte->functions) == 1);
	rtf = linitial(rte->functions);

	/*
	 * Recursively create Plan from Path for subquery.  Since we are entering
	 * a different planner context (subroot), recurse to create_plan not
	 * create_plan_recurse.
	 */
	subplan = create_plan(rel->subroot, best_path->subpath, root->curSlice);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		process_subquery_nestloop_params(root,
										 rel->subplan_params);
	}

	/* Create the TableFunctionScan plan */
	tablefunc = make_tablefunction(tlist, scan_clauses, subplan, scan_relid, rtf);

	/* Cost is determined largely by the cost of the underlying subplan */
	copy_generic_path_info(&tablefunc->scan.plan, &best_path->path);

	return tablefunc;
}

/*
 * create_valuesscan_plan
 *	 Returns a valuesscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static ValuesScan *
create_valuesscan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	ValuesScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	List	   *values_lists;

	/* it should be a values base rel... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_VALUES);
	values_lists = rte->values_lists;

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		/* The values lists could contain nestloop params, too */
		values_lists = (List *)
			replace_nestloop_params(root, (Node *) values_lists);
	}

	scan_plan = make_valuesscan(tlist, scan_clauses, scan_relid,
								values_lists);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_ctescan_plan
 *	 Returns a ctescan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static Plan *
create_ctescan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses)
{
	Plan	   *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	CtePlanInfo *cteplaninfo;
	int			planinfo_id;
	PlannerInfo *cteroot;
	Index		levelsup;
	int			ndx;
	ListCell   *lc;
	Plan	   *subplan;

	Assert(best_path->parent->rtekind == RTE_CTE);
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_CTE);
	Assert(!rte->self_reference);

	/*
	 * Find the referenced CTE, and locate the SubPlan previously made for it.
	 */
	levelsup = rte->ctelevelsup;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* shouldn't happen */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}

	/*
	 * Note: cte_plan_ids can be shorter than cteList, if we are still working
	 * on planning the CTEs (ie, this is a side-reference from another CTE).
	 * So we mustn't use forboth here.
	 */
	ndx = 0;
	foreach(lc, cteroot->parse->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (strcmp(cte->ctename, rte->ctename) == 0)
			break;
		ndx++;
	}
	if (lc == NULL)				/* shouldn't happen */
		elog(ERROR, "could not find CTE \"%s\"", rte->ctename);

	/*
	 * In PostgreSQL, we use the index to look up the plan ID in the
	 * cteroot->cte_plan_ids list. In GPDB, CTE plans work differently, and
	 * we look up the CtePlanInfo struct in the list_cteplaninfo instead.
	 */
	planinfo_id = ndx;

	if (planinfo_id < 0 || planinfo_id >= list_length(cteroot->list_cteplaninfo))
		elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);

	Assert(list_length(cteroot->list_cteplaninfo) > planinfo_id);
	cteplaninfo = list_nth(cteroot->list_cteplaninfo, planinfo_id);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	/*
	 * If this CTE is not shared, then we have a pre-made sub-Path in the CtePath.
	 */
	if (((CtePath *) best_path)->subpath)
	{
		/*
		 * Recursively create Plan from Path for subquery.  Since we are entering
		 * a different planner context (subroot), recurse to create_plan not
		 * create_plan_recurse.
		 */
		subplan = create_plan(best_path->parent->subroot, ((CtePath *) best_path)->subpath, root->curSlice);
	}
	else
	{
		/*
		 * This is a shared CTE. On first call, turn the sub-Path into a Plan, and store
		 * it in CtePlanInfo.
		 */
		if (!cteplaninfo->shared_plan)
		{
			RelOptInfo *sub_final_rel;

			sub_final_rel = fetch_upper_rel(best_path->parent->subroot, UPPERREL_FINAL, NULL);
			subplan = create_plan(best_path->parent->subroot, sub_final_rel->cheapest_total_path, root->curSlice);
			cteplaninfo->shared_plan = prepare_plan_for_sharing(cteroot, subplan);
		}
		/* Wrap the common Plan tree in a ShareInputScan node */
		subplan = share_prepared_plan(cteroot, cteplaninfo->shared_plan);
	}

	scan_plan = (Plan *) make_subqueryscan(tlist,
										   scan_clauses,
										   scan_relid,
										   subplan);

	copy_generic_path_info(scan_plan, best_path);

	return scan_plan;
}

/*
 * create_worktablescan_plan
 *	 Returns a worktablescan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static WorkTableScan *
create_worktablescan_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses)
{
	WorkTableScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	Index		levelsup;
	PlannerInfo *cteroot;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_CTE);
	Assert(rte->self_reference);

	/*
	 * We need to find the worktable param ID, which is in the plan level
	 * that's processing the recursive UNION, which is one level *below* where
	 * the CTE comes from.
	 */
	levelsup = rte->ctelevelsup;
	if (levelsup == 0)			/* shouldn't happen */
		elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	levelsup--;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* shouldn't happen */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}
	if (cteroot->wt_param_id < 0)		/* shouldn't happen */
		elog(ERROR, "could not find param ID for CTE \"%s\"", rte->ctename);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_worktablescan(tlist, scan_clauses, scan_relid,
								   cteroot->wt_param_id);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_foreignscan_plan
 *	 Returns a foreignscan plan for the relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static ForeignScan *
create_foreignscan_plan(PlannerInfo *root, ForeignPath *best_path,
						List *tlist, List *scan_clauses)
{
	ForeignScan *scan_plan;
	RelOptInfo *rel = best_path->path.parent;
	Index		scan_relid = rel->relid;
	Oid			rel_oid = InvalidOid;
	Plan	   *outer_plan = NULL;

	Assert(rel->fdwroutine != NULL);

	/* transform the child path if any */
	if (best_path->fdw_outerpath)
		outer_plan = create_plan_recurse(root, best_path->fdw_outerpath,
										 CP_EXACT_TLIST);

	/*
	 * If we're scanning a base relation, fetch its OID.  (Irrelevant if
	 * scanning a join relation.)
	 */
	if (scan_relid > 0)
	{
		RangeTblEntry *rte;

		Assert(rel->rtekind == RTE_RELATION);
		rte = planner_rt_fetch(scan_relid, root);
		Assert(rte->rtekind == RTE_RELATION);
		rel_oid = rte->relid;
	}

	/*
	 * Sort clauses into best execution order.  We do this first since the FDW
	 * might have more info than we do and wish to adjust the ordering.
	 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/*
	 * Let the FDW perform its processing on the restriction clauses and
	 * generate the plan node.  Note that the FDW might remove restriction
	 * clauses that it intends to execute remotely, or even add more (if it
	 * has selected some join clauses for remote use but also wants them
	 * rechecked locally).
	 */
	scan_plan = rel->fdwroutine->GetForeignPlan(root, rel, rel_oid,
												best_path,
												tlist, scan_clauses,
												outer_plan);

	/* Copy cost data from Path to Plan; no need to make FDW do this */
	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	/* Copy foreign server OID; likewise, no need to make FDW do this */
	scan_plan->fs_server = rel->serverid;

	/* Likewise, copy the relids that are represented by this foreign scan */
	scan_plan->fs_relids = best_path->path.parent->relids;

	/*
	 * If this is a foreign join, and to make it valid to push down we had to
	 * assume that the current user is the same as some user explicitly named
	 * in the query, mark the finished plan as depending on the current user.
	 */
	if (rel->useridiscurrent)
		root->glob->dependsOnRole = true;

	/*
	 * Replace any outer-relation variables with nestloop params in the qual,
	 * fdw_exprs and fdw_recheck_quals expressions.  We do this last so that
	 * the FDW doesn't have to be involved.  (Note that parts of fdw_exprs or
	 * fdw_recheck_quals could have come from join clauses, so doing this
	 * beforehand on the scan_clauses wouldn't work.)  We assume
	 * fdw_scan_tlist contains no such variables.
	 */
	if (best_path->path.param_info)
	{
		scan_plan->scan.plan.qual = (List *)
			replace_nestloop_params(root, (Node *) scan_plan->scan.plan.qual);
		scan_plan->fdw_exprs = (List *)
			replace_nestloop_params(root, (Node *) scan_plan->fdw_exprs);
		scan_plan->fdw_recheck_quals = (List *)
			replace_nestloop_params(root,
									(Node *) scan_plan->fdw_recheck_quals);
	}

	/*
	 * If rel is a base relation, detect whether any system columns are
	 * requested from the rel.  (If rel is a join relation, rel->relid will be
	 * 0, but there can be no Var with relid 0 in the rel's targetlist or the
	 * restriction clauses, so we skip this in that case.  Note that any such
	 * columns in base relations that were joined are assumed to be contained
	 * in fdw_scan_tlist.)	This is a bit of a kluge and might go away
	 * someday, so we intentionally leave it out of the API presented to FDWs.
	 */
	scan_plan->fsSystemCol = false;
	if (scan_relid > 0)
	{
		Bitmapset  *attrs_used = NULL;
		ListCell   *lc;
		int			i;

		/*
		 * First, examine all the attributes needed for joins or final output.
		 * Note: we must look at rel's targetlist, not the attr_needed data,
		 * because attr_needed isn't computed for inheritance child rels.
		 */
		pull_varattnos((Node *) rel->reltarget->exprs, scan_relid, &attrs_used);

		/* Add all the attributes used by restriction clauses. */
		foreach(lc, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			pull_varattnos((Node *) rinfo->clause, scan_relid, &attrs_used);
		}

		/* Now, are any system columns requested from rel? */
		for (i = FirstLowInvalidHeapAttributeNumber + 1; i < 0; i++)
		{
			if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber, attrs_used))
			{
				scan_plan->fsSystemCol = true;
				break;
			}
		}

		bms_free(attrs_used);
	}

	return scan_plan;
}

static Expr *
remove_isnotfalse_expr(Expr *expr)
{
	if (IsA(expr, BooleanTest))
	{
		BooleanTest *bt = (BooleanTest *) expr;

		if (bt->booltesttype == IS_NOT_FALSE)
		{
			return bt->arg;
		}
	}
	return expr;
}

/*
 * remove_isnotfalse
 *	  Given a list of joinclauses, extract the bare clauses, removing any IS_NOT_FALSE
 *	  additions. The original data structure is not touched; a modified list is returned
 */
static List *
remove_isnotfalse(List *clauses)
{
	List *t_list = NIL;
	ListCell *l;

	foreach(l, clauses)
	{
		Node *node = (Node *) lfirst(l);

		if (IsA(node, Expr) || IsA(node, BooleanTest))
		{
			Expr *expr = (Expr *) node;

			expr = remove_isnotfalse_expr(expr);
			t_list = lappend(t_list, expr);
		} else if (IsA(node, RestrictInfo))
		{
			RestrictInfo *restrictinfo = (RestrictInfo *) node;
			Expr *rclause = restrictinfo->clause;

			rclause = remove_isnotfalse_expr(rclause);
			t_list = lappend(t_list, rclause);
		} else
		{
			t_list = lappend(t_list, node);
		}
	}
	return t_list;
}
/*
 * create_custom_plan
 *
 * Transform a CustomPath into a Plan.
 */
static CustomScan *
create_customscan_plan(PlannerInfo *root, CustomPath *best_path,
					   List *tlist, List *scan_clauses)
{
	CustomScan *cplan;
	RelOptInfo *rel = best_path->path.parent;
	List	   *custom_plans = NIL;
	ListCell   *lc;

	/* Recursively transform child paths. */
	foreach(lc, best_path->custom_paths)
	{
		Plan	   *plan = create_plan_recurse(root, (Path *) lfirst(lc),
											   CP_EXACT_TLIST);

		custom_plans = lappend(custom_plans, plan);
	}

	/*
	 * Sort clauses into the best execution order, although custom-scan
	 * provider can reorder them again.
	 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/*
	 * Invoke custom plan provider to create the Plan node represented by the
	 * CustomPath.
	 */
	cplan = (CustomScan *) best_path->methods->PlanCustomPath(root,
															  rel,
															  best_path,
															  tlist,
															  scan_clauses,
															  custom_plans);
	Assert(IsA(cplan, CustomScan));

	/*
	 * Copy cost data from Path to Plan; no need to make custom-plan providers
	 * do this
	 */
	copy_generic_path_info(&cplan->scan.plan, &best_path->path);

	/* Likewise, copy the relids that are represented by this custom scan */
	cplan->custom_relids = best_path->path.parent->relids;

	/*
	 * Replace any outer-relation variables with nestloop params in the qual
	 * and custom_exprs expressions.  We do this last so that the custom-plan
	 * provider doesn't have to be involved.  (Note that parts of custom_exprs
	 * could have come from join clauses, so doing this beforehand on the
	 * scan_clauses wouldn't work.)  We assume custom_scan_tlist contains no
	 * such variables.
	 */
	if (best_path->path.param_info)
	{
		cplan->scan.plan.qual = (List *)
			replace_nestloop_params(root, (Node *) cplan->scan.plan.qual);
		cplan->custom_exprs = (List *)
			replace_nestloop_params(root, (Node *) cplan->custom_exprs);
	}

	return cplan;
}


/*****************************************************************************
 *
 *	JOIN METHODS
 *
 *****************************************************************************/

static NestLoop *
create_nestloop_plan(PlannerInfo *root,
					 NestPath *best_path)
{
	NestLoop   *join_plan;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	List	   *joinrestrictclauses = best_path->joinrestrictinfo;
	List	   *joinclauses;
	List	   *otherclauses;
	Relids		outerrelids;
	List	   *nestParams;
	Relids		saveOuterRels = root->curOuterRels;
	ListCell   *cell;
	ListCell   *prev;
	ListCell   *next;

	bool		prefetch = false;

#if  0
	/*
	 * If the inner path is a nestloop inner indexscan, it might be using some
	 * of the join quals as index quals, in which case we don't have to check
	 * them again at the join node.  Remove any join quals that are redundant.
	 */
	joinrestrictclauses =
		select_nonredundant_join_clauses(root,
										 joinrestrictclauses,
										 best_path->innerjoinpath);
#endif

	/* NestLoop can project, so no need to be picky about child tlists */
	outer_plan = create_plan_recurse(root, best_path->outerjoinpath, 0);

	/* For a nestloop, include outer relids in curOuterRels for inner side */
	root->curOuterRels = bms_union(root->curOuterRels,
								   best_path->outerjoinpath->parent->relids);

	inner_plan = create_plan_recurse(root, best_path->innerjoinpath, 0);

	/*
	 * MPP-1459: subqueries are resolved after our deadlock checks in
	 * pathnode.c; so we have to check here to make sure that we catch all
	 * motion deadlocks.
	 *
	 * MPP-1487: if there is already a materialize node here, we don't want to
	 * insert another one. :-)
	 *
	 * NOTE: materialize_finished_plan() does *almost* what we want -- except
	 * we aren't finished.
	 */
	if (best_path->innerjoinpath->motionHazard ||
		!best_path->innerjoinpath->rescannable)
	{
		Plan	   *p;
		Material   *mat;

		p = inner_plan;
		while (IsA(p, PartitionSelector))
			p = p->lefttree;
		if (IsA(p, Material))
		{
			mat = (Material *) p;
		}
		else
		{
			Path		matpath;	/* dummy for cost fixup */

			/* Set cost data */
			cost_material(&matpath,
						  root,
						  inner_plan->startup_cost,
						  inner_plan->total_cost,
						  inner_plan->plan_rows,
						  inner_plan->plan_width);

			mat = make_material(inner_plan);

			mat->plan.startup_cost = matpath.startup_cost;
			mat->plan.total_cost = matpath.total_cost;
			mat->plan.plan_rows = inner_plan->plan_rows;
			mat->plan.plan_width = inner_plan->plan_width;

			inner_plan = (Plan *) mat;
		}

		/*
		 * MPP-1657: Even if there is already a materialize here, we
		 * may need to update its strictness.
		 */
		if (best_path->outerjoinpath->motionHazard)
		{
			mat->cdb_strict = true;
			prefetch = true;
		}
	}
	
	/* Restore curOuterRels */
	bms_free(root->curOuterRels);
	root->curOuterRels = saveOuterRels;

	/* Sort join qual clauses into best execution order */
	joinrestrictclauses = order_qual_clauses(root, joinrestrictclauses);

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jointype))
	{
		extract_actual_join_clauses(joinrestrictclauses,
									best_path->path.parent->relids,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinrestrictclauses, false);
		otherclauses = NIL;
	}

	if (best_path->jointype == JOIN_LASJ_NOTIN)
	{
		joinclauses = remove_isnotfalse(joinclauses);
	}

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * Identify any nestloop parameters that should be supplied by this join
	 * node, and move them from root->curOuterParams to the nestParams list.
	 */
	outerrelids = best_path->outerjoinpath->parent->relids;
	nestParams = NIL;
	prev = NULL;
	for (cell = list_head(root->curOuterParams); cell; cell = next)
	{
		NestLoopParam *nlp = (NestLoopParam *) lfirst(cell);

		next = lnext(cell);
		if (IsA(nlp->paramval, Var) &&
			bms_is_member(nlp->paramval->varno, outerrelids))
		{
			root->curOuterParams = list_delete_cell(root->curOuterParams,
													cell, prev);
			nestParams = lappend(nestParams, nlp);
		}
		else if (IsA(nlp->paramval, PlaceHolderVar) &&
				 bms_overlap(((PlaceHolderVar *) nlp->paramval)->phrels,
							 outerrelids) &&
				 bms_is_subset(find_placeholder_info(root,
											(PlaceHolderVar *) nlp->paramval,
													 false)->ph_eval_at,
							   outerrelids))
		{
			root->curOuterParams = list_delete_cell(root->curOuterParams,
													cell, prev);
			nestParams = lappend(nestParams, nlp);
		}
		else
			prev = cell;
	}

	join_plan = make_nestloop(tlist,
							  joinclauses,
							  otherclauses,
							  nestParams,
							  outer_plan,
							  inner_plan,
							  best_path->jointype);

	copy_generic_path_info(&join_plan->join.plan, &best_path->path);

	if (IsA(best_path->innerjoinpath, MaterialPath))
	{
		MaterialPath *mp = (MaterialPath *) best_path->innerjoinpath;

		if (mp->cdb_strict)
			prefetch = true;
	}

	if (prefetch)
		join_plan->join.prefetch_inner = true;

	/*
	 * A motion deadlock can also happen when outer and joinqual both contain
	 * motions.  It is not easy to check for joinqual here, so we set the
	 * prefetch_joinqual mark only according to outer motion, and check for
	 * joinqual later in the executor.
	 *
	 * See ExecPrefetchJoinQual() for details.
	 */
	if (best_path->outerjoinpath &&
		best_path->outerjoinpath->motionHazard)
		join_plan->join.prefetch_joinqual = true;

	return join_plan;
}

static MergeJoin *
create_mergejoin_plan(PlannerInfo *root,
					  MergePath *best_path)
{
	MergeJoin  *join_plan;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *tlist = build_path_tlist(root, &best_path->jpath.path);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *mergeclauses;
	bool		prefetch = false;
	bool		set_mat_cdb_strict = false;
	List	   *outerpathkeys;
	List	   *innerpathkeys;
	int			nClauses;
	Oid		   *mergefamilies;
	Oid		   *mergecollations;
	int		   *mergestrategies;
	bool	   *mergenullsfirst;
	PathKey    *opathkey;
	EquivalenceClass *opeclass;
	int			i;
	ListCell   *lc;
	ListCell   *lop;
	ListCell   *lip;

	/*
	 * MergeJoin can project, so we don't have to demand exact tlists from the
	 * inputs.  However, if we're intending to sort an input's result, it's
	 * best to request a small tlist so we aren't sorting more data than
	 * necessary.
	 */
	outer_plan = create_plan_recurse(root, best_path->jpath.outerjoinpath,
					 (best_path->outersortkeys != NIL) ? CP_SMALL_TLIST : 0);

	inner_plan = create_plan_recurse(root, best_path->jpath.innerjoinpath,
					 (best_path->innersortkeys != NIL) ? CP_SMALL_TLIST : 0);

	/* Sort join qual clauses into best execution order */
	/* NB: do NOT reorder the mergeclauses */
	joinclauses = order_qual_clauses(root, best_path->jpath.joinrestrictinfo);

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		extract_actual_join_clauses(joinclauses,
									best_path->jpath.path.parent->relids,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinclauses, false);
		otherclauses = NIL;
	}

	/*
	 * Remove the mergeclauses from the list of join qual clauses, leaving the
	 * list of quals that must be checked as qpquals.
	 */
	mergeclauses = get_actual_clauses(best_path->path_mergeclauses);
	joinclauses = list_difference(joinclauses, mergeclauses);

	/*
	 * Replace any outer-relation variables with nestloop params.  There
	 * should not be any in the mergeclauses.
	 */
	if (best_path->jpath.path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * Rearrange mergeclauses, if needed, so that the outer variable is always
	 * on the left; mark the mergeclause restrictinfos with correct
	 * outer_is_left status.
	 */
	mergeclauses = get_switched_clauses(best_path->path_mergeclauses,
							 best_path->jpath.outerjoinpath->parent->relids);

	/*
	 * Create explicit sort nodes for the outer and inner paths if necessary.
	 */
	if (best_path->outersortkeys)
	{
		Sort	   *sort = make_sort_from_pathkeys(outer_plan,
												   best_path->outersortkeys,
												   true);

		label_sort_with_costsize(root, sort, -1.0);
		outer_plan = (Plan *) sort;
		outerpathkeys = best_path->outersortkeys;
	}
	else
		outerpathkeys = best_path->jpath.outerjoinpath->pathkeys;

	if (best_path->innersortkeys)
	{
		Sort	   *sort = make_sort_from_pathkeys(inner_plan,
												   best_path->innersortkeys,
												   true);

		label_sort_with_costsize(root, sort, -1.0);
		inner_plan = (Plan *) sort;
		innerpathkeys = best_path->innersortkeys;
	}
	else
		innerpathkeys = best_path->jpath.innerjoinpath->pathkeys;

	/*
	 * MPP-3300: very similar to the nested-loop join motion deadlock cases. But we may have already
	 * put some slackening operators below (e.g. a sort).
	 *
	 * We need some kind of strict slackening operator (something which consumes all of its
	 * input before producing a row of output) for our inner. And we need to prefetch that side
	 * first.
	 *
	 * See motion_sanity_walker() for details on how a deadlock may occur.
	 */
	if (best_path->jpath.outerjoinpath->motionHazard && best_path->jpath.innerjoinpath->motionHazard)
	{
		prefetch = true;
		if (!IsA(inner_plan, Sort))
		{
			if (!IsA(inner_plan, Material))
				best_path->materialize_inner = true;
			set_mat_cdb_strict = true;
		}
	}

	/*
	 * If specified, add a materialize node to shield the inner plan from the
	 * need to handle mark/restore.
	 */
	if (best_path->materialize_inner)
	{
		Plan	   *matplan = (Plan *) make_material(inner_plan);

		Assert(!IsA(inner_plan, Material));

		/*
		 * We assume the materialize will not spill to disk, and therefore
		 * charge just cpu_operator_cost per tuple.  (Keep this estimate in
		 * sync with final_cost_mergejoin.)
		 */
		copy_plan_costsize(matplan, inner_plan);
		matplan->total_cost += cpu_operator_cost * matplan->plan_rows;

		inner_plan = matplan;
	}

	if (set_mat_cdb_strict)
		((Material *) inner_plan)->cdb_strict = true;

	/*
	 * Compute the opfamily/collation/strategy/nullsfirst arrays needed by the
	 * executor.  The information is in the pathkeys for the two inputs, but
	 * we need to be careful about the possibility of mergeclauses sharing a
	 * pathkey, as well as the possibility that the inner pathkeys are not in
	 * an order matching the mergeclauses.
	 */
	nClauses = list_length(mergeclauses);
	Assert(nClauses == list_length(best_path->path_mergeclauses));
	mergefamilies = (Oid *) palloc(nClauses * sizeof(Oid));
	mergecollations = (Oid *) palloc(nClauses * sizeof(Oid));
	mergestrategies = (int *) palloc(nClauses * sizeof(int));
	mergenullsfirst = (bool *) palloc(nClauses * sizeof(bool));

	opathkey = NULL;
	opeclass = NULL;
	lop = list_head(outerpathkeys);
	lip = list_head(innerpathkeys);
	i = 0;
	foreach(lc, best_path->path_mergeclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		EquivalenceClass *oeclass;
		EquivalenceClass *ieclass;
		PathKey    *ipathkey = NULL;
		EquivalenceClass *ipeclass = NULL;
		bool		first_inner_match = false;

		/* fetch outer/inner eclass from mergeclause */
		Assert(IsA(rinfo, RestrictInfo));
		if (rinfo->outer_is_left)
		{
			oeclass = rinfo->left_ec;
			ieclass = rinfo->right_ec;
		}
		else
		{
			oeclass = rinfo->right_ec;
			ieclass = rinfo->left_ec;
		}
		Assert(oeclass != NULL);
		Assert(ieclass != NULL);

		/*
		 * We must identify the pathkey elements associated with this clause
		 * by matching the eclasses (which should give a unique match, since
		 * the pathkey lists should be canonical).  In typical cases the merge
		 * clauses are one-to-one with the pathkeys, but when dealing with
		 * partially redundant query conditions, things are more complicated.
		 *
		 * lop and lip reference the first as-yet-unmatched pathkey elements.
		 * If they're NULL then all pathkey elements have been matched.
		 *
		 * The ordering of the outer pathkeys should match the mergeclauses,
		 * by construction (see find_mergeclauses_for_outer_pathkeys()). There
		 * could be more than one mergeclause for the same outer pathkey, but
		 * no pathkey may be entirely skipped over.
		 */
		if (oeclass != opeclass)	/* multiple matches are not interesting */
		{
			/* doesn't match the current opathkey, so must match the next */
			if (lop == NULL)
				elog(ERROR, "outer pathkeys do not match mergeclauses");
			opathkey = (PathKey *) lfirst(lop);
			opeclass = opathkey->pk_eclass;
			lop = lnext(lop);
			if (oeclass != opeclass)
				elog(ERROR, "outer pathkeys do not match mergeclauses");
		}

		/*
		 * The inner pathkeys likewise should not have skipped-over keys, but
		 * it's possible for a mergeclause to reference some earlier inner
		 * pathkey if we had redundant pathkeys.  For example we might have
		 * mergeclauses like "o.a = i.x AND o.b = i.y AND o.c = i.x".  The
		 * implied inner ordering is then "ORDER BY x, y, x", but the pathkey
		 * mechanism drops the second sort by x as redundant, and this code
		 * must cope.
		 *
		 * It's also possible for the implied inner-rel ordering to be like
		 * "ORDER BY x, y, x DESC".  We still drop the second instance of x as
		 * redundant; but this means that the sort ordering of a redundant
		 * inner pathkey should not be considered significant.  So we must
		 * detect whether this is the first clause matching an inner pathkey.
		 */
		if (lip)
		{
			ipathkey = (PathKey *) lfirst(lip);
			ipeclass = ipathkey->pk_eclass;
			if (ieclass == ipeclass)
			{
				/* successful first match to this inner pathkey */
				lip = lnext(lip);
				first_inner_match = true;
			}
		}
		if (!first_inner_match)
		{
			/* redundant clause ... must match something before lip */
			ListCell   *l2;

			foreach(l2, innerpathkeys)
			{
				if (l2 == lip)
					break;
				ipathkey = (PathKey *) lfirst(l2);
				ipeclass = ipathkey->pk_eclass;
				if (ieclass == ipeclass)
					break;
			}
			if (ieclass != ipeclass)
				elog(ERROR, "inner pathkeys do not match mergeclauses");
		}

		/*
		 * The pathkeys should always match each other as to opfamily and
		 * collation (which affect equality), but if we're considering a
		 * redundant inner pathkey, its sort ordering might not match.  In
		 * such cases we may ignore the inner pathkey's sort ordering and use
		 * the outer's.  (In effect, we're lying to the executor about the
		 * sort direction of this inner column, but it does not matter since
		 * the run-time row comparisons would only reach this column when
		 * there's equality for the earlier column containing the same eclass.
		 * There could be only one value in this column for the range of inner
		 * rows having a given value in the earlier column, so it does not
		 * matter which way we imagine this column to be ordered.)  But a
		 * non-redundant inner pathkey had better match outer's ordering too.
		 */
		if (opathkey->pk_opfamily != ipathkey->pk_opfamily ||
			opathkey->pk_eclass->ec_collation != ipathkey->pk_eclass->ec_collation)
			elog(ERROR, "left and right pathkeys do not match in mergejoin");
		if (first_inner_match &&
			(opathkey->pk_strategy != ipathkey->pk_strategy ||
			 opathkey->pk_nulls_first != ipathkey->pk_nulls_first))
			elog(ERROR, "left and right pathkeys do not match in mergejoin");

		/* OK, save info for executor */
		mergefamilies[i] = opathkey->pk_opfamily;
		mergecollations[i] = opathkey->pk_eclass->ec_collation;
		mergestrategies[i] = opathkey->pk_strategy;
		mergenullsfirst[i] = opathkey->pk_nulls_first;
		i++;
	}

	/*
	 * Note: it is not an error if we have additional pathkey elements (i.e.,
	 * lop or lip isn't NULL here).  The input paths might be better-sorted
	 * than we need for the current mergejoin.
	 */

	/*
	 * Now we can build the mergejoin node.
	 */
	join_plan = make_mergejoin(tlist,
							   joinclauses,
							   otherclauses,
							   mergeclauses,
							   mergefamilies,
							   mergecollations,
							   mergestrategies,
							   mergenullsfirst,
							   outer_plan,
							   inner_plan,
							   best_path->jpath.jointype);

	join_plan->join.prefetch_inner = prefetch;

	/*
	 * A motion deadlock can also happen when outer and joinqual both contain
	 * motions.  It is not easy to check for joinqual here, so we set the
	 * prefetch_joinqual mark only according to outer motion, and check for
	 * joinqual later in the executor.
	 *
	 * See ExecPrefetchJoinQual() for details.
	 */
	if (best_path->jpath.outerjoinpath &&
		best_path->jpath.outerjoinpath->motionHazard)
		join_plan->join.prefetch_joinqual = true;
	/*
	 * If inner motion is not under a Material or Sort node then there could
	 * also be motion deadlock between inner and joinqual in mergejoin.
	 */
	if (best_path->jpath.innerjoinpath &&
		best_path->jpath.innerjoinpath->motionHazard)
		join_plan->join.prefetch_joinqual = true;

	/* Costs of sort and material steps are included in path cost already */
	copy_generic_path_info(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}

static HashJoin *
create_hashjoin_plan(PlannerInfo *root,
					 HashPath *best_path)
{
	HashJoin   *join_plan;
	Hash	   *hash_plan;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *tlist = build_path_tlist(root, &best_path->jpath.path);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *hashclauses;
	Oid			skewTable = InvalidOid;
	AttrNumber	skewColumn = InvalidAttrNumber;
	bool		skewInherit = false;
	Oid			skewColType = InvalidOid;
	int32		skewColTypmod = -1;

	/*
	 * HashJoin can project, so we don't have to demand exact tlists from the
	 * inputs.  However, it's best to request a small tlist from the inner
	 * side, so that we aren't storing more data than necessary.  Likewise, if
	 * we anticipate batching, request a small tlist from the outer side so
	 * that we don't put extra data in the outer batch files.
	 */
	outer_plan = create_plan_recurse(root, best_path->jpath.outerjoinpath,
						  (best_path->num_batches > 1) ? CP_SMALL_TLIST : 0);

	inner_plan = create_plan_recurse(root, best_path->jpath.innerjoinpath,
									 CP_SMALL_TLIST);

	/* Sort join qual clauses into best execution order */
	joinclauses = order_qual_clauses(root, best_path->jpath.joinrestrictinfo);
	/* There's no point in sorting the hash clauses ... */

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		extract_actual_join_clauses(joinclauses,
									best_path->jpath.path.parent->relids,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinclauses, false);
		otherclauses = NIL;
	}

	/*
	 * Remove the hashclauses from the list of join qual clauses, leaving the
	 * list of quals that must be checked as qpquals.
	 */
	hashclauses = get_actual_clauses(best_path->path_hashclauses);
	joinclauses = list_difference(joinclauses, hashclauses);

	/*
	 * Replace any outer-relation variables with nestloop params.  There
	 * should not be any in the hashclauses.
	 */
	if (best_path->jpath.path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * Rearrange hashclauses, if needed, so that the outer variable is always
	 * on the left.
	 */
	hashclauses = get_switched_clauses(best_path->path_hashclauses,
							 best_path->jpath.outerjoinpath->parent->relids);

	/*
	 * If there is a single join clause and we can identify the outer variable
	 * as a simple column reference, supply its identity for possible use in
	 * skew optimization.  (Note: in principle we could do skew optimization
	 * with multiple join clauses, but we'd have to be able to determine the
	 * most common combinations of outer values, which we don't currently have
	 * enough stats for.)
	 */
	if (list_length(hashclauses) == 1)
	{
		OpExpr	   *clause = (OpExpr *) linitial(hashclauses);
		Node	   *node;

		Assert(is_opclause(clause));
		node = (Node *) linitial(clause->args);
		if (IsA(node, RelabelType))
			node = (Node *) ((RelabelType *) node)->arg;
		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;
			RangeTblEntry *rte;

			rte = root->simple_rte_array[var->varno];
			if (rte->rtekind == RTE_RELATION)
			{
				skewTable = rte->relid;
				skewColumn = var->varattno;
				skewInherit = rte->inh;
				skewColType = var->vartype;
				skewColTypmod = var->vartypmod;
			}
		}
	}

	/*
	 * Build the hash node and hash join node.
	 */
	hash_plan = make_hash(inner_plan,
						  skewTable,
						  skewColumn,
						  skewInherit,
						  skewColType,
						  skewColTypmod);

	/*
	 * Set Hash node's startup & total costs equal to total cost of input
	 * plan; this only affects EXPLAIN display not decisions.
	 */
	copy_plan_costsize(&hash_plan->plan, inner_plan);
	hash_plan->plan.startup_cost = hash_plan->plan.total_cost;

	join_plan = make_hashjoin(tlist,
							  joinclauses,
							  otherclauses,
							  hashclauses,
							  NIL, /* hashqualclauses */
							  outer_plan,
							  (Plan *) hash_plan,
							  best_path->jpath.jointype);

	/*
	 * MPP-4635.  best_path->jpath.outerjoinpath may be NULL.
	 * From the comment, it is adaptive nestloop join may cause this.
	 */
	/*
	 * MPP-4165: we need to descend left-first if *either* of the
	 * subplans have any motion.
	 */
	/*
	 * MPP-3300: unify motion-deadlock prevention for all join types.
	 * This allows us to undo the MPP-989 changes in nodeHashjoin.c
	 * (allowing us to check the outer for rows before building the
	 * hash-table).
	 */
	if (best_path->jpath.outerjoinpath == NULL ||
		best_path->jpath.outerjoinpath->motionHazard ||
		best_path->jpath.innerjoinpath->motionHazard)
	{
		join_plan->join.prefetch_inner = true;
	}

	/*
	 * A motion deadlock can also happen when outer and joinqual both contain
	 * motions.  It is not easy to check for joinqual here, so we set the
	 * prefetch_joinqual mark only according to outer motion, and check for
	 * joinqual later in the executor.
	 *
	 * See ExecPrefetchJoinQual() for details.
	 */
	if (best_path->jpath.outerjoinpath &&
		best_path->jpath.outerjoinpath->motionHazard)
		join_plan->join.prefetch_joinqual = true;

	copy_generic_path_info(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}


/*****************************************************************************
 *
 *	SUPPORTING ROUTINES
 *
 *****************************************************************************/

/*
 * replace_nestloop_params
 *	  Replace outer-relation Vars and PlaceHolderVars in the given expression
 *	  with nestloop Params
 *
 * All Vars and PlaceHolderVars belonging to the relation(s) identified by
 * root->curOuterRels are replaced by Params, and entries are added to
 * root->curOuterParams if not already present.
 */
static Node *
replace_nestloop_params(PlannerInfo *root, Node *expr)
{
	/* No setup needed for tree walk, so away we go */
	return replace_nestloop_params_mutator(expr, root);
}

static Node *
replace_nestloop_params_mutator(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Param	   *param;
		NestLoopParam *nlp;
		ListCell   *lc;

		/* Upper-level Vars should be long gone at this point */
		Assert(var->varlevelsup == 0);
		/* If not to be replaced, we can just return the Var unmodified */
		if (!bms_is_member(var->varno, root->curOuterRels))
			return node;
		/* Create a Param representing the Var */
		param = assign_nestloop_param_var(root, var);
		/* Is this param already listed in root->curOuterParams? */
		foreach(lc, root->curOuterParams)
		{
			nlp = (NestLoopParam *) lfirst(lc);
			if (nlp->paramno == param->paramid)
			{
				Assert(equal(var, nlp->paramval));
				/* Present, so we can just return the Param */
				return (Node *) param;
			}
		}
		/* No, so add it */
		nlp = makeNode(NestLoopParam);
		nlp->paramno = param->paramid;
		nlp->paramval = var;
		root->curOuterParams = lappend(root->curOuterParams, nlp);
		/* And return the replacement Param */
		return (Node *) param;
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;
		Param	   *param;
		NestLoopParam *nlp;
		ListCell   *lc;

		/* Upper-level PlaceHolderVars should be long gone at this point */
		Assert(phv->phlevelsup == 0);

		/*
		 * Check whether we need to replace the PHV.  We use bms_overlap as a
		 * cheap/quick test to see if the PHV might be evaluated in the outer
		 * rels, and then grab its PlaceHolderInfo to tell for sure.
		 */
		if (!bms_overlap(phv->phrels, root->curOuterRels) ||
		  !bms_is_subset(find_placeholder_info(root, phv, false)->ph_eval_at,
						 root->curOuterRels))
		{
			/*
			 * We can't replace the whole PHV, but we might still need to
			 * replace Vars or PHVs within its expression, in case it ends up
			 * actually getting evaluated here.  (It might get evaluated in
			 * this plan node, or some child node; in the latter case we don't
			 * really need to process the expression here, but we haven't got
			 * enough info to tell if that's the case.)  Flat-copy the PHV
			 * node and then recurse on its expression.
			 *
			 * Note that after doing this, we might have different
			 * representations of the contents of the same PHV in different
			 * parts of the plan tree.  This is OK because equal() will just
			 * match on phid/phlevelsup, so setrefs.c will still recognize an
			 * upper-level reference to a lower-level copy of the same PHV.
			 */
			PlaceHolderVar *newphv = makeNode(PlaceHolderVar);

			memcpy(newphv, phv, sizeof(PlaceHolderVar));
			newphv->phexpr = (Expr *)
				replace_nestloop_params_mutator((Node *) phv->phexpr,
												root);
			return (Node *) newphv;
		}
		/* Create a Param representing the PlaceHolderVar */
		param = assign_nestloop_param_placeholdervar(root, phv);
		/* Is this param already listed in root->curOuterParams? */
		foreach(lc, root->curOuterParams)
		{
			nlp = (NestLoopParam *) lfirst(lc);
			if (nlp->paramno == param->paramid)
			{
				Assert(equal(phv, nlp->paramval));
				/* Present, so we can just return the Param */
				return (Node *) param;
			}
		}
		/* No, so add it */
		nlp = makeNode(NestLoopParam);
		nlp->paramno = param->paramid;
		nlp->paramval = (Var *) phv;
		root->curOuterParams = lappend(root->curOuterParams, nlp);
		/* And return the replacement Param */
		return (Node *) param;
	}
	return expression_tree_mutator(node,
								   replace_nestloop_params_mutator,
								   (void *) root);
}

/*
 * process_subquery_nestloop_params
 *	  Handle params of a parameterized subquery that need to be fed
 *	  from an outer nestloop.
 *
 * Currently, that would be *all* params that a subquery in FROM has demanded
 * from the current query level, since they must be LATERAL references.
 *
 * The subplan's references to the outer variables are already represented
 * as PARAM_EXEC Params, so we need not modify the subplan here.  What we
 * do need to do is add entries to root->curOuterParams to signal the parent
 * nestloop plan node that it must provide these values.
 */
static void
process_subquery_nestloop_params(PlannerInfo *root, List *subplan_params)
{
	ListCell   *ppl;

	foreach(ppl, subplan_params)
	{
		PlannerParamItem *pitem = (PlannerParamItem *) lfirst(ppl);

		if (IsA(pitem->item, Var))
		{
			Var		   *var = (Var *) pitem->item;
			NestLoopParam *nlp;
			ListCell   *lc;

			/* If not from a nestloop outer rel, complain */
			if (!bms_is_member(var->varno, root->curOuterRels))
				elog(ERROR, "non-LATERAL parameter required by subquery");
			/* Is this param already listed in root->curOuterParams? */
			foreach(lc, root->curOuterParams)
			{
				nlp = (NestLoopParam *) lfirst(lc);
				if (nlp->paramno == pitem->paramId)
				{
					Assert(equal(var, nlp->paramval));
					/* Present, so nothing to do */
					break;
				}
			}
			if (lc == NULL)
			{
				/* No, so add it */
				nlp = makeNode(NestLoopParam);
				nlp->paramno = pitem->paramId;
				nlp->paramval = copyObject(var);
				root->curOuterParams = lappend(root->curOuterParams, nlp);
			}
		}
		else if (IsA(pitem->item, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) pitem->item;
			NestLoopParam *nlp;
			ListCell   *lc;

			/* If not from a nestloop outer rel, complain */
			if (!bms_is_subset(find_placeholder_info(root, phv, false)->ph_eval_at,
							   root->curOuterRels))
				elog(ERROR, "non-LATERAL parameter required by subquery");
			/* Is this param already listed in root->curOuterParams? */
			foreach(lc, root->curOuterParams)
			{
				nlp = (NestLoopParam *) lfirst(lc);
				if (nlp->paramno == pitem->paramId)
				{
					Assert(equal(phv, nlp->paramval));
					/* Present, so nothing to do */
					break;
				}
			}
			if (lc == NULL)
			{
				/* No, so add it */
				nlp = makeNode(NestLoopParam);
				nlp->paramno = pitem->paramId;
				nlp->paramval = copyObject(phv);
				root->curOuterParams = lappend(root->curOuterParams, nlp);
			}
		}
		else
			elog(ERROR, "unexpected type of subquery parameter");
	}
}

/*
 * fix_indexqual_references
 *	  Adjust indexqual clauses to the form the executor's indexqual
 *	  machinery needs.
 *
 * We have four tasks here:
 *	* Remove RestrictInfo nodes from the input clauses.
 *	* Replace any outer-relation Var or PHV nodes with nestloop Params.
 *	  (XXX eventually, that responsibility should go elsewhere?)
 *	* Index keys must be represented by Var nodes with varattno set to the
 *	  index's attribute number, not the attribute number in the original rel.
 *	* If the index key is on the right, commute the clause to put it on the
 *	  left.
 *
 * The result is a modified copy of the path's indexquals list --- the
 * original is not changed.  Note also that the copy shares no substructure
 * with the original; this is needed in case there is a subplan in it (we need
 * two separate copies of the subplan tree, or things will go awry).
 */
static List *
fix_indexqual_references(PlannerInfo *root, IndexPath *index_path)
{
	IndexOptInfo *index = index_path->indexinfo;
	List	   *fixed_indexquals;
	ListCell   *lcc,
			   *lci;

	fixed_indexquals = NIL;

	forboth(lcc, index_path->indexquals, lci, index_path->indexqualcols)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lcc);
		int			indexcol = lfirst_int(lci);
		Node	   *clause;

		Assert(IsA(rinfo, RestrictInfo));

		/*
		 * Replace any outer-relation variables with nestloop params.
		 *
		 * This also makes a copy of the clause, so it's safe to modify it
		 * in-place below.
		 */
		clause = replace_nestloop_params(root, (Node *) rinfo->clause);

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;

			if (list_length(op->args) != 2)
				elog(ERROR, "indexqual clause is not binary opclause");

			/*
			 * Check to see if the indexkey is on the right; if so, commute
			 * the clause.  The indexkey should be the side that refers to
			 * (only) the base relation.
			 */
			if (!bms_equal(rinfo->left_relids, index->rel->relids))
				CommuteOpExpr(op);

			/*
			 * Now replace the indexkey expression with an index Var.
			 */
			linitial(op->args) = fix_indexqual_operand(linitial(op->args),
													   index,
													   indexcol);
		}
		else if (IsA(clause, RowCompareExpr))
		{
			RowCompareExpr *rc = (RowCompareExpr *) clause;
			Expr	   *newrc;
			List	   *indexcolnos;
			bool		var_on_left;
			ListCell   *lca,
					   *lcai;

			/*
			 * Re-discover which index columns are used in the rowcompare.
			 */
			newrc = adjust_rowcompare_for_index(rc,
												index,
												indexcol,
												&indexcolnos,
												&var_on_left);

			/*
			 * Trouble if adjust_rowcompare_for_index thought the
			 * RowCompareExpr didn't match the index as-is; the clause should
			 * have gone through that routine already.
			 */
			if (newrc != (Expr *) rc)
				elog(ERROR, "inconsistent results from adjust_rowcompare_for_index");

			/*
			 * Check to see if the indexkey is on the right; if so, commute
			 * the clause.
			 */
			if (!var_on_left)
				CommuteRowCompareExpr(rc);

			/*
			 * Now replace the indexkey expressions with index Vars.
			 */
			Assert(list_length(rc->largs) == list_length(indexcolnos));
			forboth(lca, rc->largs, lcai, indexcolnos)
			{
				lfirst(lca) = fix_indexqual_operand(lfirst(lca),
													index,
													lfirst_int(lcai));
			}
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

			/* Never need to commute... */

			/* Replace the indexkey expression with an index Var. */
			linitial(saop->args) = fix_indexqual_operand(linitial(saop->args),
														 index,
														 indexcol);
		}
		else if (IsA(clause, NullTest))
		{
			NullTest   *nt = (NullTest *) clause;

			/* Replace the indexkey expression with an index Var. */
			nt->arg = (Expr *) fix_indexqual_operand((Node *) nt->arg,
													 index,
													 indexcol);
		}
		else
			elog(ERROR, "unsupported indexqual type: %d",
				 (int) nodeTag(clause));

		fixed_indexquals = lappend(fixed_indexquals, clause);
	}

	return fixed_indexquals;
}

/*
 * fix_indexorderby_references
 *	  Adjust indexorderby clauses to the form the executor's index
 *	  machinery needs.
 *
 * This is a simplified version of fix_indexqual_references.  The input does
 * not have RestrictInfo nodes, and we assume that indxpath.c already
 * commuted the clauses to put the index keys on the left.  Also, we don't
 * bother to support any cases except simple OpExprs, since nothing else
 * is allowed for ordering operators.
 */
static List *
fix_indexorderby_references(PlannerInfo *root, IndexPath *index_path)
{
	IndexOptInfo *index = index_path->indexinfo;
	List	   *fixed_indexorderbys;
	ListCell   *lcc,
			   *lci;

	fixed_indexorderbys = NIL;

	forboth(lcc, index_path->indexorderbys, lci, index_path->indexorderbycols)
	{
		Node	   *clause = (Node *) lfirst(lcc);
		int			indexcol = lfirst_int(lci);

		/*
		 * Replace any outer-relation variables with nestloop params.
		 *
		 * This also makes a copy of the clause, so it's safe to modify it
		 * in-place below.
		 */
		clause = replace_nestloop_params(root, clause);

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;

			if (list_length(op->args) != 2)
				elog(ERROR, "indexorderby clause is not binary opclause");

			/*
			 * Now replace the indexkey expression with an index Var.
			 */
			linitial(op->args) = fix_indexqual_operand(linitial(op->args),
													   index,
													   indexcol);
		}
		else
			elog(ERROR, "unsupported indexorderby type: %d",
				 (int) nodeTag(clause));

		fixed_indexorderbys = lappend(fixed_indexorderbys, clause);
	}

	return fixed_indexorderbys;
}

/*
 * fix_indexqual_operand
 *	  Convert an indexqual expression to a Var referencing the index column.
 *
 * We represent index keys by Var nodes having varno == INDEX_VAR and varattno
 * equal to the index's attribute number (index column position).
 *
 * Most of the code here is just for sanity cross-checking that the given
 * expression actually matches the index column it's claimed to.
 */
static Node *
fix_indexqual_operand(Node *node, IndexOptInfo *index, int indexcol)
{
	Var		   *result;
	int			pos;
	ListCell   *indexpr_item;

	/*
	 * Remove any binary-compatible relabeling of the indexkey
	 */
	if (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	Assert(indexcol >= 0 && indexcol < index->ncolumns);

	if (index->indexkeys[indexcol] != 0)
	{
		/* It's a simple index column */
		if (IsA(node, Var) &&
			((Var *) node)->varno == index->rel->relid &&
			((Var *) node)->varattno == index->indexkeys[indexcol])
		{
			result = (Var *) copyObject(node);
			result->varno = INDEX_VAR;
			result->varattno = indexcol + 1;
			return (Node *) result;
		}
		else
			elog(ERROR, "index key does not match expected index column");
	}

	/* It's an index expression, so find and cross-check the expression */
	indexpr_item = list_head(index->indexprs);
	for (pos = 0; pos < index->ncolumns; pos++)
	{
		if (index->indexkeys[pos] == 0)
		{
			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			if (pos == indexcol)
			{
				Node	   *indexkey;

				indexkey = (Node *) lfirst(indexpr_item);
				if (indexkey && IsA(indexkey, RelabelType))
					indexkey = (Node *) ((RelabelType *) indexkey)->arg;
				if (equal(node, indexkey))
				{
					result = makeVar(INDEX_VAR, indexcol + 1,
									 exprType(lfirst(indexpr_item)), -1,
									 exprCollation(lfirst(indexpr_item)),
									 0);
					return (Node *) result;
				}
				else
					elog(ERROR, "index key does not match expected index column");
			}
			indexpr_item = lnext(indexpr_item);
		}
	}

	/* Ooops... */
	elog(ERROR, "index key does not match expected index column");
	return NULL;				/* keep compiler quiet */
}

/*
 * get_switched_clauses
 *	  Given a list of merge or hash joinclauses (as RestrictInfo nodes),
 *	  extract the bare clauses, and rearrange the elements within the
 *	  clauses, if needed, so the outer join variable is on the left and
 *	  the inner is on the right.  The original clause data structure is not
 *	  touched; a modified list is returned.  We do, however, set the transient
 *	  outer_is_left field in each RestrictInfo to show which side was which.
 */
static List *
get_switched_clauses(List *clauses, Relids outerrelids)
{
	List	   *t_list = NIL;
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);

		Expr	   *rclause = restrictinfo->clause;
		OpExpr	   *clause;

		/**
		 * If this is a IS NOT FALSE boolean test, we can peek underneath.
		 */
		if (IsA(rclause, BooleanTest))
		{
			BooleanTest *bt = (BooleanTest *) rclause;

			if (bt->booltesttype == IS_NOT_FALSE)
			{
				rclause = bt->arg;
			}
		}

		Assert(is_opclause(rclause));
		clause = (OpExpr *) rclause;
		if (bms_is_subset(restrictinfo->right_relids, outerrelids))
		{
			/*
			 * Duplicate just enough of the structure to allow commuting the
			 * clause without changing the original list.  Could use
			 * copyObject, but a complete deep copy is overkill.
			 */
			OpExpr	   *temp = makeNode(OpExpr);

			temp->opno = clause->opno;
			temp->opfuncid = InvalidOid;
			temp->opresulttype = clause->opresulttype;
			temp->opretset = clause->opretset;
			temp->opcollid = clause->opcollid;
			temp->inputcollid = clause->inputcollid;
			temp->args = list_copy(clause->args);
			temp->location = clause->location;
			/* Commute it --- note this modifies the temp node in-place. */
			CommuteOpExpr(temp);
			t_list = lappend(t_list, temp);
			restrictinfo->outer_is_left = false;
		}
		else
		{
			Assert(bms_is_subset(restrictinfo->left_relids, outerrelids));
			t_list = lappend(t_list, clause);
			restrictinfo->outer_is_left = true;
		}
	}
	return t_list;
}

/*
 * order_qual_clauses
 *		Given a list of qual clauses that will all be evaluated at the same
 *		plan node, sort the list into the order we want to check the quals
 *		in at runtime.
 *
 * Ideally the order should be driven by a combination of execution cost and
 * selectivity, but it's not immediately clear how to account for both,
 * and given the uncertainty of the estimates the reliability of the decisions
 * would be doubtful anyway.  So we just order by estimated per-tuple cost,
 * being careful not to change the order when (as is often the case) the
 * estimates are identical.
 *
 * Although this will work on either bare clauses or RestrictInfos, it's
 * much faster to apply it to RestrictInfos, since it can re-use cost
 * information that is cached in RestrictInfos.
 *
 * Note: some callers pass lists that contain entries that will later be
 * removed; this is the easiest way to let this routine see RestrictInfos
 * instead of bare clauses.  It's OK because we only sort by cost, but
 * a cost/selectivity combination would likely do the wrong thing.
 */
static List *
order_qual_clauses(PlannerInfo *root, List *clauses)
{
	typedef struct
	{
		Node	   *clause;
		Cost		cost;
	} QualItem;
	int			nitems = list_length(clauses);
	QualItem   *items;
	ListCell   *lc;
	int			i;
	List	   *result;

	/* No need to work hard for 0 or 1 clause */
	if (nitems <= 1)
		return clauses;

	/*
	 * Collect the items and costs into an array.  This is to avoid repeated
	 * cost_qual_eval work if the inputs aren't RestrictInfos.
	 */
	items = (QualItem *) palloc(nitems * sizeof(QualItem));
	i = 0;
	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);
		QualCost	qcost;

		cost_qual_eval_node(&qcost, clause, root);
		items[i].clause = clause;
		items[i].cost = qcost.per_tuple;
		i++;
	}

	/*
	 * Sort.  We don't use qsort() because it's not guaranteed stable for
	 * equal keys.  The expected number of entries is small enough that a
	 * simple insertion sort should be good enough.
	 */
	for (i = 1; i < nitems; i++)
	{
		QualItem	newitem = items[i];
		int			j;

		/* insert newitem into the already-sorted subarray */
		for (j = i; j > 0; j--)
		{
			if (newitem.cost >= items[j - 1].cost)
				break;
			items[j] = items[j - 1];
		}
		items[j] = newitem;
	}

	/* Convert back to a list */
	result = NIL;
	for (i = 0; i < nitems; i++)
		result = lappend(result, items[i].clause);

	return result;
}

/*
 * Copy cost and size info from a Path node to the Plan node created from it.
 * The executor usually won't use this info, but it's needed by EXPLAIN.
 * Also copy the parallel-aware flag, which the executor *will* use.
 */
static void
copy_generic_path_info(Plan *dest, Path *src)
{
	dest->startup_cost = src->startup_cost;
	dest->total_cost = src->total_cost;
	dest->plan_rows = src->rows;
	dest->plan_width = src->pathtarget->width;
	dest->parallel_aware = src->parallel_aware;
}

/*
 * Copy cost and size info from a lower plan node to an inserted node.
 * (Most callers alter the info after copying it.)
 */
static void
copy_plan_costsize(Plan *dest, Plan *src)
{
	dest->startup_cost = src->startup_cost;
	dest->total_cost = src->total_cost;
	dest->plan_rows = src->plan_rows;
	dest->plan_width = src->plan_width;
	/* Assume the inserted node is not parallel-aware. */
	dest->parallel_aware = false;
}

/*
 * Some places in this file build Sort nodes that don't have a directly
 * corresponding Path node.  The cost of the sort is, or should have been,
 * included in the cost of the Path node we're working from, but since it's
 * not split out, we have to re-figure it using cost_sort().  This is just
 * to label the Sort node nicely for EXPLAIN.
 *
 * limit_tuples is as for cost_sort (in particular, pass -1 if no limit)
 */
static void
label_sort_with_costsize(PlannerInfo *root, Sort *plan, double limit_tuples)
{
	Plan	   *lefttree = plan->plan.lefttree;
	Path		sort_path;		/* dummy for result of cost_sort */

	cost_sort(&sort_path, root, NIL,
			  lefttree->total_cost,
			  lefttree->plan_rows,
			  lefttree->plan_width,
			  0.0,
			  work_mem,
			  limit_tuples);
	plan->plan.startup_cost = sort_path.startup_cost;
	plan->plan.total_cost = sort_path.total_cost;
	plan->plan.plan_rows = lefttree->plan_rows;
	plan->plan.plan_width = lefttree->plan_width;
	plan->plan.parallel_aware = false;
}


/*****************************************************************************
 *
 *	PLAN NODE BUILDING ROUTINES
 *
 * In general, these functions are not passed the original Path and therefore
 * leave it to the caller to fill in the cost/width fields from the Path,
 * typically by calling copy_generic_path_info().  This convention is
 * somewhat historical, but it does support a few places above where we build
 * a plan node without having an exactly corresponding Path node.  Under no
 * circumstances should one of these functions do its own cost calculations,
 * as that would be redundant with calculations done while building Paths.
 *
 *****************************************************************************/

static SeqScan *
make_seqscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid)
{
	SeqScan    *node = makeNode(SeqScan);
	Plan	   *plan = &node->plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scanrelid = scanrelid;

	return node;
}

static ExternalScan *
make_externalscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  List *urilist,
				  char *fmtoptstring,
				  char fmttype,
				  bool ismasteronly,
				  int rejectlimit,
				  bool rejectlimitinrows,
				  bool logerrors,
				  int encoding)
{
	ExternalScan *node = makeNode(ExternalScan);
	Plan	   *plan = &node->scan.plan;
	static uint32 scancounter = 0;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;

	/* external specifictions */
	node->uriList = urilist;
	node->fmtOptString = fmtoptstring;
	node->fmtType = fmttype;
	node->isMasterOnly = ismasteronly;
	node->rejLimit = rejectlimit;
	node->rejLimitInRows = rejectlimitinrows;
	node->logErrors = logerrors;
	node->encoding = encoding;
	node->scancounter = scancounter++;

	return node;
}

static SampleScan *
make_samplescan(List *qptlist,
				List *qpqual,
				Index scanrelid,
				TableSampleClause *tsc)
{
	SampleScan *node = makeNode(SampleScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tablesample = tsc;

	return node;
}

static IndexScan *
make_indexscan(List *qptlist,
			   List *qpqual,
			   Index scanrelid,
			   Oid indexid,
			   List *indexqual,
			   List *indexqualorig,
			   List *indexorderby,
			   List *indexorderbyorig,
			   List *indexorderbyops,
			   ScanDirection indexscandir)
{
	IndexScan  *node = makeNode(IndexScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;
	node->indexorderby = indexorderby;
	node->indexorderbyorig = indexorderbyorig;
	node->indexorderbyops = indexorderbyops;
	node->indexorderdir = indexscandir;

	return node;
}

static IndexOnlyScan *
make_indexonlyscan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   Oid indexid,
				   List *indexqual,
				   List *indexqualorig,
				   List *indexorderby,
				   List *indextlist,
				   ScanDirection indexscandir)
{
	IndexOnlyScan *node = makeNode(IndexOnlyScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;
	node->indexorderby = indexorderby;
	node->indextlist = indextlist;
	node->indexorderdir = indexscandir;

	return node;
}

static BitmapIndexScan *
make_bitmap_indexscan(Index scanrelid,
					  Oid indexid,
					  List *indexqual,
					  List *indexqualorig)
{
	BitmapIndexScan *node = makeNode(BitmapIndexScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = NIL;		/* not used */
	plan->qual = NIL;			/* not used */
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;

	return node;
}

static BitmapHeapScan *
make_bitmap_heapscan(List *qptlist,
					 List *qpqual,
					 Plan *lefttree,
					 List *bitmapqualorig,
					 Index scanrelid)
{
	BitmapHeapScan *node = makeNode(BitmapHeapScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->bitmapqualorig = bitmapqualorig;

	return node;
}

static TidScan *
make_tidscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 List *tidquals)
{
	TidScan    *node = makeNode(TidScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tidquals = tidquals;

	return node;
}

SubqueryScan *
make_subqueryscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  Plan *subplan)
{
	SubqueryScan *node = makeNode(SubqueryScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	plan->extParam = bms_copy(subplan->extParam);
	plan->allParam = bms_copy(subplan->allParam);

	/*
	 * Note that, in most scan nodes, scanrelid refers to an entry in the rtable of the
	 * containing plan; in a subqueryscan node, the containing plan is the higher
	 * level plan!
	 */
	node->scan.scanrelid = scanrelid;

	node->subplan = subplan;

	return node;
}

static FunctionScan *
make_functionscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  List *functions,
				  bool funcordinality)
{
	FunctionScan *node = makeNode(FunctionScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->functions = functions;
	node->funcordinality = funcordinality;

	return node;
}

static TableFunctionScan *
make_tablefunction(List *qptlist, List *qpqual, Plan *subplan,
				   Index scanrelid, RangeTblFunction *function)
{
	TableFunctionScan *node = makeNode(TableFunctionScan);
	Plan	   *plan = &node->scan.plan;

	copy_plan_costsize(plan, subplan);  /* only care about copying size */

	/* FIXME: fix costing */
	plan->startup_cost  = subplan->startup_cost;
	plan->total_cost    = subplan->total_cost;
	plan->total_cost   += 2 * plan->plan_rows;

	plan->qual			= qpqual;
	plan->targetlist	= qptlist;
	plan->righttree		= NULL;

	/* Fill in information for the subplan */
	plan->lefttree		 = subplan;
	node->scan.scanrelid = scanrelid;
	node->function = function;

	return node;
}

static ValuesScan *
make_valuesscan(List *qptlist,
				List *qpqual,
				Index scanrelid,
				List *values_lists)
{
	ValuesScan *node = makeNode(ValuesScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->values_lists = values_lists;

	return node;
}

static pg_attribute_unused() CteScan *
make_ctescan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 int ctePlanId,
			 int cteParam)
{
	CteScan    *node = makeNode(CteScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->ctePlanId = ctePlanId;
	node->cteParam = cteParam;

	return node;
}

static WorkTableScan *
make_worktablescan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   int wtParam)
{
	WorkTableScan *node = makeNode(WorkTableScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->wtParam = wtParam;

	return node;
}

ForeignScan *
make_foreignscan(List *qptlist,
				 List *qpqual,
				 Index scanrelid,
				 List *fdw_exprs,
				 List *fdw_private,
				 List *fdw_scan_tlist,
				 List *fdw_recheck_quals,
				 Plan *outer_plan)
{
	ForeignScan *node = makeNode(ForeignScan);
	Plan	   *plan = &node->scan.plan;

	/* cost will be filled in by create_foreignscan_plan */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = outer_plan;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->operation = CMD_SELECT;
	/* fs_server will be filled in by create_foreignscan_plan */
	node->fs_server = InvalidOid;
	node->fdw_exprs = fdw_exprs;
	node->fdw_private = fdw_private;
	node->fdw_scan_tlist = fdw_scan_tlist;
	node->fdw_recheck_quals = fdw_recheck_quals;
	/* fs_relids will be filled in by create_foreignscan_plan */
	node->fs_relids = NULL;
	/* fsSystemCol will be filled in by create_foreignscan_plan */
	node->fsSystemCol = false;

	return node;
}

static Append *
make_append(List *appendplans, List *tlist)
{
	Append	   *node = makeNode(Append);
	Plan	   *plan = &node->plan;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->appendplans = appendplans;

	return node;
}

static RecursiveUnion *
make_recursive_union(List *tlist,
					 Plan *lefttree,
					 Plan *righttree,
					 int wtParam,
					 List *distinctList,
					 long numGroups)
{
	RecursiveUnion *node = makeNode(RecursiveUnion);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->wtParam = wtParam;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	node->numCols = numCols;
	if (numCols > 0)
	{
		int			keyno = 0;
		AttrNumber *dupColIdx;
		Oid		   *dupOperators;
		ListCell   *slitem;

		dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
		dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);

		foreach(slitem, distinctList)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
			TargetEntry *tle = get_sortgroupclause_tle(sortcl,
													   plan->targetlist);

			dupColIdx[keyno] = tle->resno;
			dupOperators[keyno] = sortcl->eqop;
			Assert(OidIsValid(dupOperators[keyno]));
			keyno++;
		}
		node->dupColIdx = dupColIdx;
		node->dupOperators = dupOperators;
	}
	node->numGroups = numGroups;

	return node;
}

static BitmapAnd *
make_bitmap_and(List *bitmapplans)
{
	BitmapAnd  *node = makeNode(BitmapAnd);
	Plan	   *plan = &node->plan;

	plan->targetlist = NIL;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->bitmapplans = bitmapplans;

	return node;
}

static BitmapOr *
make_bitmap_or(List *bitmapplans)
{
	BitmapOr   *node = makeNode(BitmapOr);
	Plan	   *plan = &node->plan;

	plan->targetlist = NIL;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->bitmapplans = bitmapplans;

	return node;
}

NestLoop *
make_nestloop(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *nestParams,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype)
{
	NestLoop   *node = makeNode(NestLoop);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;
	node->nestParams = nestParams;

	return node;
}

HashJoin *
make_hashjoin(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *hashclauses,
			  List *hashqualclauses,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype)
{
	HashJoin   *node = makeNode(HashJoin);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->hashclauses = hashclauses;
	node->hashqualclauses = hashqualclauses;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;

	return node;
}

Hash *
make_hash(Plan *lefttree,
		  Oid skewTable,
		  AttrNumber skewColumn,
		  bool skewInherit,
		  Oid skewColType,
		  int32 skewColTypmod)
{
	Hash	   *node = makeNode(Hash);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->skewTable = skewTable;
	node->skewColumn = skewColumn;
	node->skewInherit = skewInherit;
	node->skewColType = skewColType;
	node->skewColTypmod = skewColTypmod;

	node->rescannable = false;	/* CDB (unused for now) */

	return node;
}

MergeJoin *
make_mergejoin(List *tlist,
			   List *joinclauses,
			   List *otherclauses,
			   List *mergeclauses,
			   Oid *mergefamilies,
			   Oid *mergecollations,
			   int *mergestrategies,
			   bool *mergenullsfirst,
			   Plan *lefttree,
			   Plan *righttree,
			   JoinType jointype)
{
	MergeJoin  *node = makeNode(MergeJoin);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->mergeclauses = mergeclauses;
	node->mergeFamilies = mergefamilies;
	node->mergeCollations = mergecollations;
	node->mergeStrategies = mergestrategies;
	node->mergeNullsFirst = mergenullsfirst;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;

	return node;
}

/*
 * make_sort --- basic routine to build a Sort plan node
 *
 * Caller must have built the sortColIdx, sortOperators, collations, and
 * nullsFirst arrays already.
 */
static Sort *
make_sort(Plan *lefttree, int numCols,
		  AttrNumber *sortColIdx, Oid *sortOperators,
		  Oid *collations, bool *nullsFirst)
{
	Sort	   *node = makeNode(Sort);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->numCols = numCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;
	node->collations = collations;
	node->nullsFirst = nullsFirst;

	Assert(sortColIdx[0] != 0);

	node->noduplicates = false; /* CDB */

	node->share_type = SHARE_NOTSHARED;
	node->share_id = SHARE_ID_NOT_SHARED;
	node->driver_slice = -1;
	node->nsharer = 0;
	node->nsharer_xslice = 0;

	return node;
}

/*
 * add_sort_cost --- basic routine to accumulate Sort cost into a
 * plan node representing the input cost.
 *
 * Unused arguments (e.g., sortColIdx and sortOperators arrays) are
 * included to allow for future improvements to sort costing.  Note
 * that root may be NULL (e.g. when called outside make_sort).
 */
Plan *
add_sort_cost(PlannerInfo *root, Plan *input, double limit_tuples)
{
	Path		sort_path;		/* dummy for result of cost_sort */

	cost_sort(&sort_path, root, NIL,
			  input->total_cost,
			  input->plan_rows,
			  input->plan_width,
			  0.0,
			  work_mem,
			  limit_tuples);
	input->startup_cost = sort_path.startup_cost;
	input->total_cost = sort_path.total_cost;

	return input;
}

/*
 * prepare_sort_from_pathkeys
 *	  Prepare to sort according to given pathkeys
 *
 * This is used to set up for both Sort and MergeAppend nodes.  It calculates
 * the executor's representation of the sort key information, and adjusts the
 * plan targetlist if needed to add resjunk sort columns.
 *
 * Input parameters:
 *	  'lefttree' is the plan node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *	  'relids' identifies the child relation being sorted, if any
 *	  'reqColIdx' is NULL or an array of required sort key column numbers
 *	  'adjust_tlist_in_place' is TRUE if lefttree must be modified in-place
 *
 * We must convert the pathkey information into arrays of sort key column
 * numbers, sort operator OIDs, collation OIDs, and nulls-first flags,
 * which is the representation the executor wants.  These are returned into
 * the output parameters *p_numsortkeys etc.
 *
 * When looking for matches to an EquivalenceClass's members, we will only
 * consider child EC members if they match 'relids'.  This protects against
 * possible incorrect matches to child expressions that contain no Vars.
 *
 * If reqColIdx isn't NULL then it contains sort key column numbers that
 * we should match.  This is used when making child plans for a MergeAppend;
 * it's an error if we can't match the columns.
 *
 * If the pathkeys include expressions that aren't simple Vars, we will
 * usually need to add resjunk items to the input plan's targetlist to
 * compute these expressions, since the Sort/MergeAppend node itself won't
 * do any such calculations.  If the input plan type isn't one that can do
 * projections, this means adding a Result node just to do the projection.
 * However, the caller can pass adjust_tlist_in_place = TRUE to force the
 * lefttree tlist to be modified in-place regardless of whether the node type
 * can project --- we use this for fixing the tlist of MergeAppend itself.
 *
 * Returns the node which is to be the input to the Sort (either lefttree,
 * or a Result stacked atop lefttree).
 */
static Plan *
prepare_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
						   Relids relids,
						   const AttrNumber *reqColIdx,
						   bool adjust_tlist_in_place,
						   int *p_numsortkeys,
						   AttrNumber **p_sortColIdx,
						   Oid **p_sortOperators,
						   Oid **p_collations,
						   bool **p_nullsFirst,
						   bool add_keys_to_targetlist)
{
	List	   *tlist = lefttree->targetlist;
	ListCell   *i;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/*
	 * We will need at most list_length(pathkeys) sort columns; possibly less
	 */
	numsortkeys = list_length(pathkeys);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;

	foreach(i, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(i);
		EquivalenceClass *ec = pathkey->pk_eclass;
		EquivalenceMember *em;
		TargetEntry *tle = NULL;
		Oid			pk_datatype = InvalidOid;
		Oid			sortop;
		ListCell   *j;

		if (ec->ec_has_volatile)
		{
			/*
			 * If the pathkey's EquivalenceClass is volatile, then it must
			 * have come from an ORDER BY clause, and we have to match it to
			 * that same targetlist entry.
			 */
			if (ec->ec_sortref == 0)	/* can't happen */
				elog(ERROR, "volatile EquivalenceClass has no sortref");
			tle = get_sortgroupref_tle(ec->ec_sortref, tlist);
			Assert(tle);
			Assert(list_length(ec->ec_members) == 1);
			pk_datatype = ((EquivalenceMember *) linitial(ec->ec_members))->em_datatype;
		}
		else if (reqColIdx != NULL)
		{
			/*
			 * If we are given a sort column number to match, only consider
			 * the single TLE at that position.  It's possible that there is
			 * no such TLE, in which case fall through and generate a resjunk
			 * targetentry (we assume this must have happened in the parent
			 * plan as well).  If there is a TLE but it doesn't match the
			 * pathkey's EC, we do the same, which is probably the wrong thing
			 * but we'll leave it to caller to complain about the mismatch.
			 */
			tle = get_tle_by_resno(tlist, reqColIdx[numsortkeys]);
			if (tle)
			{
				em = find_ec_member_for_tle(ec, tle, relids);
				if (em)
				{
					/* found expr at right place in tlist */
					pk_datatype = em->em_datatype;
				}
				else
					tle = NULL;
			}
		}
		else
		{
			/*
			 * Otherwise, we can sort by any non-constant expression listed in
			 * the pathkey's EquivalenceClass.  For now, we take the first
			 * tlist item found in the EC. If there's no match, we'll generate
			 * a resjunk entry using the first EC member that is an expression
			 * in the input's vars.  (The non-const restriction only matters
			 * if the EC is below_outer_join; but if it isn't, it won't
			 * contain consts anyway, else we'd have discarded the pathkey as
			 * redundant.)
			 *
			 * XXX if we have a choice, is there any way of figuring out which
			 * might be cheapest to execute?  (For example, int4lt is likely
			 * much cheaper to execute than numericlt, but both might appear
			 * in the same equivalence class...)  Not clear that we ever will
			 * have an interesting choice in practice, so it may not matter.
			 */
			foreach(j, tlist)
			{
				tle = (TargetEntry *) lfirst(j);
				em = find_ec_member_for_tle(ec, tle, relids);
				if (em)
				{
					/* found expr already in tlist */
					pk_datatype = em->em_datatype;
					break;
				}
				tle = NULL;
			}
		}

		if (!tle)
		{
			/*
			 * No matching tlist item; look for a computable expression. Note
			 * that we treat Aggrefs as if they were variables; this is
			 * necessary when attempting to sort the output from an Agg node
			 * for use in a WindowFunc (since grouping_planner will have
			 * treated the Aggrefs as variables, too).  Likewise, if we find a
			 * WindowFunc in a sort expression, treat it as a variable.
			 */
			Expr	   *sortexpr = NULL;

			if (!add_keys_to_targetlist)
				break;

			foreach(j, ec->ec_members)
			{
				EquivalenceMember *em = (EquivalenceMember *) lfirst(j);
				List	   *exprvars;
				ListCell   *k;

				/*
				 * We shouldn't be trying to sort by an equivalence class that
				 * contains a constant, so no need to consider such cases any
				 * further.
				 */
				if (em->em_is_const)
					continue;

				/*
				 * Ignore child members unless they match the rel being
				 * sorted.
				 */
				if (em->em_is_child &&
					!bms_equal(em->em_relids, relids))
					continue;

				sortexpr = em->em_expr;
				exprvars = pull_var_clause((Node *) sortexpr,
										   PVC_INCLUDE_AGGREGATES |
										   PVC_INCLUDE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);
				foreach(k, exprvars)
				{
					if (!tlist_member_ignore_relabel(lfirst(k), tlist))
						break;
				}
				list_free(exprvars);
				if (!k)
				{
					pk_datatype = em->em_datatype;
					break;		/* found usable expression */
				}
			}
			if (!j)
				elog(ERROR, "could not find pathkey item to sort");

			/*
			 * Do we need to insert a Result node?
			 */
			if (!adjust_tlist_in_place &&
				!is_projection_capable_plan(lefttree))
			{
				/* copy needed so we don't modify input's tlist below */
				tlist = copyObject(tlist);
				lefttree = inject_projection_plan(lefttree, tlist);
			}

			/* Don't bother testing is_projection_capable_plan again */
			adjust_tlist_in_place = true;

			/*
			 * Add resjunk entry to input's tlist
			 */
			tle = makeTargetEntry(sortexpr,
								  list_length(tlist) + 1,
								  NULL,
								  true);
			tlist = lappend(tlist, tle);
			lefttree->targetlist = tlist;		/* just in case NIL before */
		}

		/*
		 * Look up the correct sort operator from the PathKey's slightly
		 * abstracted representation.
		 */
		sortop = get_opfamily_member(pathkey->pk_opfamily,
									 pk_datatype,
									 pk_datatype,
									 pathkey->pk_strategy);
		if (!OidIsValid(sortop))	/* should not happen */
			elog(ERROR, "could not find member %d(%u,%u) of opfamily %u",
				 pathkey->pk_strategy, pk_datatype, pk_datatype,
				 pathkey->pk_opfamily);

		/* Add the column to the sort arrays */
		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = sortop;
		collations[numsortkeys] = ec->ec_collation;
		nullsFirst[numsortkeys] = pathkey->pk_nulls_first;
		numsortkeys++;
	}

	/* Return results */
	*p_numsortkeys = numsortkeys;
	*p_sortColIdx = sortColIdx;
	*p_sortOperators = sortOperators;
	*p_collations = collations;
	*p_nullsFirst = nullsFirst;

	return lefttree;
}

/*
 * find_ec_member_for_tle
 *		Locate an EquivalenceClass member matching the given TLE, if any
 *
 * Child EC members are ignored unless they match 'relids'.
 */
static EquivalenceMember *
find_ec_member_for_tle(EquivalenceClass *ec,
					   TargetEntry *tle,
					   Relids relids)
{
	Expr	   *tlexpr;
	ListCell   *lc;

	/* We ignore binary-compatible relabeling on both ends */
	tlexpr = tle->expr;
	while (tlexpr && IsA(tlexpr, RelabelType))
		tlexpr = ((RelabelType *) tlexpr)->arg;

	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		Expr	   *emexpr;

		/*
		 * We shouldn't be trying to sort by an equivalence class that
		 * contains a constant, so no need to consider such cases any further.
		 */
		if (em->em_is_const)
			continue;

		/*
		 * Ignore child members unless they match the rel being sorted.
		 */
		if (em->em_is_child &&
			!bms_equal(em->em_relids, relids))
			continue;

		/* Match if same expression (after stripping relabel) */
		emexpr = em->em_expr;
		while (emexpr && IsA(emexpr, RelabelType))
			emexpr = ((RelabelType *) emexpr)->arg;

		if (equal(emexpr, tlexpr))
			return em;
	}

	return NULL;
}

/*
 * make_sort_from_pathkeys
 *	  Create sort plan to sort according to given pathkeys
 *
 *	  'lefttree' is the node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *	  'add_keys_to_targetlist' is true if it is ok to append to the subplan's
 *				targetlist or insert a Result node atop the subplan to
 *				evaluate sort key exprs that are not already present in the
 *				subplan's tlist.
 */
Sort *
make_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
						bool add_keys_to_targetlist)
{
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Compute sort column info, and adjust lefttree as needed */
	lefttree = prepare_sort_from_pathkeys(lefttree, pathkeys,
										  NULL,
										  NULL,
										  false,
										  &numsortkeys,
										  &sortColIdx,
										  &sortOperators,
										  &collations,
										  &nullsFirst,
										  add_keys_to_targetlist);

	if (lefttree == NULL)
	{
		Assert(!add_keys_to_targetlist);
		return NULL;
	}

	/* Now build the Sort node */
	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

/*
 * make_sort_from_sortclauses
 *	  Create sort plan to sort according to given sortclauses
 *
 *	  'sortcls' is a list of SortGroupClauses
 *	  'lefttree' is the node which yields input tuples
 */
Sort *
make_sort_from_sortclauses(List *sortcls, Plan *lefttree)
{
	List	   *sub_tlist = lefttree->targetlist;
	ListCell   *l;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Convert list-ish representation to arrays wanted by executor */
	numsortkeys = list_length(sortcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;
	foreach(l, sortcls)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, sub_tlist);

		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = sortcl->sortop;
		collations[numsortkeys] = exprCollation((Node *) tle->expr);
		nullsFirst[numsortkeys] = sortcl->nulls_first;
		numsortkeys++;
	}

	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

/*
 * make_sort_from_groupcols
 *	  Create sort plan to sort based on grouping columns
 *
 * 'groupcls' is the list of SortGroupClauses
 * 'grpColIdx' gives the column numbers to use
 *
 * This might look like it could be merged with make_sort_from_sortclauses,
 * but presently we *must* use the grpColIdx[] array to locate sort columns,
 * because the child plan's tlist is not marked with ressortgroupref info
 * appropriate to the grouping node.  So, only the sort ordering info
 * is used from the SortGroupClause entries.
 */
Sort *
make_sort_from_groupcols(List *groupcls,
						 AttrNumber *grpColIdx,
						 Plan *lefttree)
{
	List	   *sub_tlist = lefttree->targetlist;
	ListCell   *l;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Convert list-ish representation to arrays wanted by executor */
	numsortkeys = list_length(groupcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;

	foreach(l, groupcls)
	{
		SortGroupClause *grpcl = (SortGroupClause *) lfirst(l);
		TargetEntry *tle = get_tle_by_resno(sub_tlist, grpColIdx[numsortkeys]);

		if (!tle)
			elog(ERROR, "could not retrieve tle for sort-from-groupcols");

		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = grpcl->sortop;
		collations[numsortkeys] = exprCollation((Node *) tle->expr);
		nullsFirst[numsortkeys] = grpcl->nulls_first;
		numsortkeys++;
	}

	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

/* --------------------------------------------------------------------
 * make_motion -- creates a Motion node.
 * Caller must have built the pHashDefn, pFixedDefn,
 * and pSortDefn structs already.
 * This call only make a motion node, without filling in flow info
 * After calling this function, caller need to call add_slice_to_motion
 * --------------------------------------------------------------------
 */
Motion *
make_motion(PlannerInfo *root, Plan *lefttree,
			int numSortCols, AttrNumber *sortColIdx,
			Oid *sortOperators, Oid *collations, bool *nullsFirst)
{
    Motion *node = makeNode(Motion);
    Plan   *plan = &node->plan;

	Assert(lefttree);
	Assert(!IsA(lefttree, Motion));

	plan->startup_cost = lefttree->startup_cost;
	plan->total_cost = lefttree->total_cost;
	plan->plan_rows = lefttree->plan_rows;
	plan->plan_width = lefttree->plan_width;

	if (IsA(lefttree, ModifyTable))
	{
		ModifyTable *mtplan = (ModifyTable *) lefttree;

		/* See setrefs.c. A ModifyTable doesn't have a valid targetlist */
		if (mtplan->returningLists)
			plan->targetlist = linitial(mtplan->returningLists);
		else
			plan->targetlist = NIL;
	}
	else
	{
		plan->targetlist = lefttree->targetlist;
	}

	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->numSortCols = numSortCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;
	node->collations = collations;
	node->nullsFirst = nullsFirst;

#ifdef USE_ASSERT_CHECKING
	/*
	 * If the child node was a Sort, then surely the order the caller gave us
	 * must match that of the underlying sort.
	 */
	if (numSortCols > 0 && IsA(lefttree, Sort))
	{
		Sort	   *childsort = (Sort *) lefttree;
		Assert(childsort->numCols >= node->numSortCols);
		Assert(memcmp(childsort->sortColIdx, node->sortColIdx, node->numSortCols * sizeof(AttrNumber)) == 0);
		Assert(memcmp(childsort->sortOperators, node->sortOperators, node->numSortCols * sizeof(Oid)) == 0);
		Assert(memcmp(childsort->nullsFirst, node->nullsFirst, node->numSortCols * sizeof(bool)) == 0);
	}
#endif

	node->sendSorted = (numSortCols > 0);

	plan->extParam = bms_copy(lefttree->extParam);
	plan->allParam = bms_copy(lefttree->allParam);

	return node;
}

Material *
make_material(Plan *lefttree)
{
	Material   *node = makeNode(Material);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->cdb_strict = false;
	node->share_type = SHARE_NOTSHARED;
	node->share_id = SHARE_ID_NOT_SHARED;
	node->driver_slice = -1;
	node->nsharer = 0;
	node->nsharer_xslice = 0;

	return node;
}

/*
 * materialize_finished_plan: stick a Material node atop a completed plan
 *
 * There are a couple of places where we want to attach a Material node
 * after completion of create_plan(), without any MaterialPath path.
 * Those places should probably be refactored someday to do this on the
 * Path representation, but it's not worth the trouble yet.
 */
Plan *
materialize_finished_plan(PlannerInfo *root, Plan *subplan)
{
	Plan	   *matplan;
	Path		matpath;		/* dummy for result of cost_material */

	matplan = (Plan *) make_material(subplan);

	/* Set cost data */
	cost_material(&matpath,
				  root,
				  subplan->startup_cost,
				  subplan->total_cost,
				  subplan->plan_rows,
				  subplan->plan_width);
	matplan->startup_cost = matpath.startup_cost;
	matplan->total_cost = matpath.total_cost;
	matplan->plan_rows = subplan->plan_rows;
	matplan->plan_width = subplan->plan_width;
	matplan->parallel_aware = false;

	/*
	 * Since this is applied after calling create_plan(), this becomes the
	 * topmost node in the (sub)plan. We have to keep the 'flow' up to date.
	 */
	matplan->flow = subplan->flow;

	return matplan;
}

Agg *
make_agg(List *tlist, List *qual,
		 AggStrategy aggstrategy, AggSplit aggsplit,
		 bool streaming,
		 int numGroupCols, AttrNumber *grpColIdx, Oid *grpOperators,
		 List *groupingSets, List *chain,
		 double dNumGroups, Plan *lefttree)
{
	Agg		   *node = makeNode(Agg);
	Plan	   *plan = &node->plan;
	long		numGroups;

	/* Reduce to long, but 'ware overflow! */
	numGroups = (long) Min(dNumGroups, (double) LONG_MAX);

	node->aggstrategy = aggstrategy;
	node->aggsplit = aggsplit;
	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->grpOperators = grpOperators;
	node->groupingSets = groupingSets;
	node->numGroups = numGroups;
	node->groupingSets = groupingSets;
	node->chain = chain;
	node->streaming = streaming;

	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	plan->extParam = bms_copy(lefttree->extParam);
	plan->allParam = bms_copy(lefttree->allParam);

	return node;
}

static WindowAgg *
make_windowagg(List *tlist, Index winref,
			   int partNumCols, AttrNumber *partColIdx, Oid *partOperators,
			   int ordNumCols, AttrNumber *ordColIdx, Oid *ordOperators,
			   AttrNumber firstOrderCol, Oid firstOrderCmpOperator, bool firstOrderNullsFirst,
			   int frameOptions, Node *startOffset, Node *endOffset,
			   Plan *lefttree)
{
	WindowAgg  *node = makeNode(WindowAgg);
	Plan	   *plan = &node->plan;

	node->winref = winref;
	node->partNumCols = partNumCols;
	node->partColIdx = partColIdx;
	node->partOperators = partOperators;
	node->ordNumCols = ordNumCols;
	node->ordColIdx = ordColIdx;
	node->ordOperators = ordOperators;
	node->firstOrderCol = firstOrderCol;
	node->firstOrderCmpOperator= firstOrderCmpOperator;
	node->firstOrderNullsFirst= firstOrderNullsFirst;
	node->frameOptions = frameOptions;
	node->startOffset = startOffset;
	node->endOffset = endOffset;

	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	/* WindowAgg nodes never have a qual clause */
	plan->qual = NIL;

	return node;
}

/*
 * distinctList is a list of SortGroupClauses, identifying the targetlist items
 * that should be considered by the Unique filter.  The input path must
 * already be sorted accordingly.
 */
static Unique *
make_unique_from_sortclauses(Plan *lefttree, List *distinctList)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);
	int			keyno = 0;
	AttrNumber *uniqColIdx;
	Oid		   *uniqOperators;
	ListCell   *slitem;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	Assert(numCols > 0);
	uniqColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	uniqOperators = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		uniqColIdx[keyno] = tle->resno;
		uniqOperators[keyno] = sortcl->eqop;
		Assert(OidIsValid(uniqOperators[keyno]));
		keyno++;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;

	/* CDB */	/* pass DISTINCT to sort */
	if (IsA(lefttree, Sort) && gp_enable_sort_distinct)
	{
		Sort	   *pSort = (Sort *) lefttree;

		pSort->noduplicates = true;
	}

	return node;
}

/*
 * as above, but use pathkeys to identify the sort columns and semantics
 */
static Unique *
make_unique_from_pathkeys(Plan *lefttree, List *pathkeys, int numCols)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;
	int			keyno = 0;
	AttrNumber *uniqColIdx;
	Oid		   *uniqOperators;
	ListCell   *lc;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * Convert pathkeys list into arrays of attr indexes and equality
	 * operators, as wanted by executor.  This has a lot in common with
	 * prepare_sort_from_pathkeys ... maybe unify sometime?
	 */
	Assert(numCols >= 0 && numCols <= list_length(pathkeys));
	uniqColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	uniqOperators = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(lc, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(lc);
		EquivalenceClass *ec = pathkey->pk_eclass;
		EquivalenceMember *em;
		TargetEntry *tle = NULL;
		Oid			pk_datatype = InvalidOid;
		Oid			eqop;
		ListCell   *j;

		/* Ignore pathkeys beyond the specified number of columns */
		if (keyno >= numCols)
			break;

		if (ec->ec_has_volatile)
		{
			/*
			 * If the pathkey's EquivalenceClass is volatile, then it must
			 * have come from an ORDER BY clause, and we have to match it to
			 * that same targetlist entry.
			 */
			if (ec->ec_sortref == 0)	/* can't happen */
				elog(ERROR, "volatile EquivalenceClass has no sortref");
			tle = get_sortgroupref_tle(ec->ec_sortref, plan->targetlist);
			Assert(tle);
			Assert(list_length(ec->ec_members) == 1);
			pk_datatype = ((EquivalenceMember *) linitial(ec->ec_members))->em_datatype;
		}
		else
		{
			/*
			 * Otherwise, we can use any non-constant expression listed in the
			 * pathkey's EquivalenceClass.  For now, we take the first tlist
			 * item found in the EC.
			 */
			foreach(j, plan->targetlist)
			{
				tle = (TargetEntry *) lfirst(j);
				em = find_ec_member_for_tle(ec, tle, NULL);
				if (em)
				{
					/* found expr already in tlist */
					pk_datatype = em->em_datatype;
					break;
				}
				tle = NULL;
			}
		}

		if (!tle)
			elog(ERROR, "could not find pathkey item to sort");

		/*
		 * Look up the correct equality operator from the PathKey's slightly
		 * abstracted representation.
		 */
		eqop = get_opfamily_member(pathkey->pk_opfamily,
								   pk_datatype,
								   pk_datatype,
								   BTEqualStrategyNumber);
		if (!OidIsValid(eqop))	/* should not happen */
			elog(ERROR, "could not find member %d(%u,%u) of opfamily %u",
				 BTEqualStrategyNumber, pk_datatype, pk_datatype,
				 pathkey->pk_opfamily);

		uniqColIdx[keyno] = tle->resno;
		uniqOperators[keyno] = eqop;

		keyno++;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;

	return node;
}

static Gather *
make_gather(List *qptlist,
			List *qpqual,
			int nworkers,
			bool single_copy,
			Plan *subplan)
{
	Gather	   *node = makeNode(Gather);
	Plan	   *plan = &node->plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->num_workers = nworkers;
	node->single_copy = single_copy;
	node->invisible = false;

	return node;
}

/*
 * distinctList is a list of SortGroupClauses, identifying the targetlist
 * items that should be considered by the SetOp filter.  The input path must
 * already be sorted accordingly.
 */
static SetOp *
make_setop(SetOpCmd cmd, SetOpStrategy strategy, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx, int firstFlag,
		   long numGroups)
{
	SetOp	   *node = makeNode(SetOp);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);
	int			keyno = 0;
	AttrNumber *dupColIdx;
	Oid		   *dupOperators;
	ListCell   *slitem;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	Assert(numCols > 0);
	dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		dupColIdx[keyno] = tle->resno;
		dupOperators[keyno] = sortcl->eqop;
		Assert(OidIsValid(dupOperators[keyno]));
		keyno++;
	}

	node->cmd = cmd;
	node->strategy = strategy;
	node->numCols = numCols;
	node->dupColIdx = dupColIdx;
	node->dupOperators = dupOperators;
	node->flagColIdx = flagColIdx;
	node->firstFlag = firstFlag;
	node->numGroups = numGroups;

	return node;
}

/*
 * make_lockrows
 *	  Build a LockRows plan node
 */
static LockRows *
make_lockrows(Plan *lefttree, List *rowMarks, int epqParam)
{
	LockRows   *node = makeNode(LockRows);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->rowMarks = rowMarks;
	node->epqParam = epqParam;

	return node;
}

/*
 * make_limit
 *	  Build a Limit plan node
 */
Limit *
make_limit(Plan *lefttree, Node *limitOffset, Node *limitCount)
{
	Limit	   *node = makeNode(Limit);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->limitOffset = limitOffset;
	node->limitCount = limitCount;

	return node;
}

/*
 * make_result
 *	  Build a Result plan node
 */
Result *
make_result(List *tlist,
			Node *resconstantqual,
			Plan *subplan)
{
	Result	   *node = makeNode(Result);
	Plan	   *plan = &node->plan;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->resconstantqual = resconstantqual;

	node->numHashFilterCols = 0;
	node->hashFilterColIdx = NULL;
	node->hashFilterFuncs = NULL;

	return node;
}

/*
 * make_repeat
 *	  Build a Repeat plan node
 */
Repeat *
make_repeat(List *tlist,
			List *qual,
			Expr *repeatCountExpr,
			uint64 grouping,
			Plan *subplan)
{
	Repeat	   *node = makeNode(Repeat);
	Plan	   *plan = &node->plan;

	Assert(subplan != NULL);
	copy_plan_costsize(plan, subplan);

	plan->targetlist = tlist;
	plan->qual = qual;
	plan->lefttree = subplan;
	plan->righttree = NULL;

	node->repeatCountExpr = repeatCountExpr;
	node->grouping = grouping;

	return node;
}

/*
 * make_modifytable
 *	  Build a ModifyTable plan node
 */
static ModifyTable *
make_modifytable(PlannerInfo *root,
				 CmdType operation, bool canSetTag,
				 Index nominalRelation,
				 List *resultRelations, List *subplans,
				 List *withCheckOptionLists, List *returningLists,
				 List *is_split_updates,
				 List *rowMarks, OnConflictExpr *onconflict, int epqParam)
{
	ModifyTable *node = makeNode(ModifyTable);
	List	   *fdw_private_list;
	Bitmapset  *direct_modify_plans;
	ListCell   *lc;
	int			i;

	Assert(list_length(resultRelations) == list_length(subplans));
	Assert(withCheckOptionLists == NIL ||
		   list_length(resultRelations) == list_length(withCheckOptionLists));
	Assert(returningLists == NIL ||
		   list_length(resultRelations) == list_length(returningLists));
	Assert(list_length(resultRelations) == list_length(is_split_updates));

	node->plan.lefttree = NULL;
	node->plan.righttree = NULL;
	node->plan.qual = NIL;
	/* setrefs.c will fill in the targetlist, if needed */
	node->plan.targetlist = NIL;

	node->operation = operation;
	node->canSetTag = canSetTag;
	node->nominalRelation = nominalRelation;
	node->resultRelations = resultRelations;
	node->resultRelIndex = -1;	/* will be set correctly in setrefs.c */
	node->plans = subplans;
	if (!onconflict)
	{
		node->onConflictAction = ONCONFLICT_NONE;
		node->onConflictSet = NIL;
		node->onConflictWhere = NULL;
		node->arbiterIndexes = NIL;
		node->exclRelRTI = 0;
		node->exclRelTlist = NIL;
	}
	else
	{
		node->onConflictAction = onconflict->action;
		node->onConflictSet = onconflict->onConflictSet;
		node->onConflictWhere = onconflict->onConflictWhere;

		/*
		 * If a set of unique index inference elements was provided (an
		 * INSERT...ON CONFLICT "inference specification"), then infer
		 * appropriate unique indexes (or throw an error if none are
		 * available).
		 */
		node->arbiterIndexes = infer_arbiter_indexes(root);

		node->exclRelRTI = onconflict->exclRelIndex;
		node->exclRelTlist = onconflict->exclRelTlist;
	}
	node->withCheckOptionLists = withCheckOptionLists;
	node->returningLists = returningLists;
	node->rowMarks = rowMarks;
	node->epqParam = epqParam;

	node->isSplitUpdates = is_split_updates;

	/*
	 * For each result relation that is a foreign table, allow the FDW to
	 * construct private plan data, and accumulate it all into a list.
	 */
	fdw_private_list = NIL;
	direct_modify_plans = NULL;
	i = 0;
	foreach(lc, resultRelations)
	{
		Index		rti = lfirst_int(lc);
		FdwRoutine *fdwroutine;
		List	   *fdw_private;
		bool		direct_modify;

		/*
		 * If possible, we want to get the FdwRoutine from our RelOptInfo for
		 * the table.  But sometimes we don't have a RelOptInfo and must get
		 * it the hard way.  (In INSERT, the target relation is not scanned,
		 * so it's not a baserel; and there are also corner cases for
		 * updatable views where the target rel isn't a baserel.)
		 */
		if (rti < root->simple_rel_array_size &&
			root->simple_rel_array[rti] != NULL)
		{
			RelOptInfo *resultRel = root->simple_rel_array[rti];

			fdwroutine = resultRel->fdwroutine;
		}
		else
		{
			RangeTblEntry *rte = planner_rt_fetch(rti, root);

			Assert(rte->rtekind == RTE_RELATION);
			if (rte->relkind == RELKIND_FOREIGN_TABLE)
				fdwroutine = GetFdwRoutineByRelId(rte->relid);
			else
				fdwroutine = NULL;
		}

		/*
		 * If the target foreign table has any row-level triggers, we can't
		 * modify the foreign table directly.
		 */
		direct_modify = false;
		if (fdwroutine != NULL &&
			fdwroutine->PlanDirectModify != NULL &&
			fdwroutine->BeginDirectModify != NULL &&
			fdwroutine->IterateDirectModify != NULL &&
			fdwroutine->EndDirectModify != NULL &&
			!has_row_triggers(root, rti, operation))
			direct_modify = fdwroutine->PlanDirectModify(root, node, rti, i);
		if (direct_modify)
			direct_modify_plans = bms_add_member(direct_modify_plans, i);

		if (!direct_modify &&
			fdwroutine != NULL &&
			fdwroutine->PlanForeignModify != NULL)
			fdw_private = fdwroutine->PlanForeignModify(root, node, rti, i);
		else
			fdw_private = NIL;
		fdw_private_list = lappend(fdw_private_list, fdw_private);
		i++;
	}
	node->fdwPrivLists = fdw_private_list;
	node->fdwDirectModifyPlans = direct_modify_plans;

	return node;
}

/*
 * is_projection_capable_path
 *		Check whether a given Path node is able to do projection.
 */
bool
is_projection_capable_path(Path *path)
{
	/* Most plan types can project, so just list the ones that can't */
	switch (path->pathtype)
	{
		case T_Hash:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_LockRows:
		case T_Limit:
		case T_ModifyTable:
		case T_MergeAppend:
		case T_RecursiveUnion:
		case T_Motion:
		case T_ShareInputScan:
			return false;
		case T_Append:

			/*
			 * Append can't project, but if it's being used to represent a
			 * dummy path, claim that it can project.  This prevents us from
			 * converting a rel from dummy to non-dummy status by applying a
			 * projection to its dummy path.
			 */
			return IS_DUMMY_PATH(path);
		default:
			break;
	}
	return true;
}

/*
 * is_projection_capable_plan
 *		Check whether a given Plan node is able to do projection.
 */
bool
is_projection_capable_plan(Plan *plan)
{
	/* Most plan types can project, so just list the ones that can't */
	switch (nodeTag(plan))
	{
		case T_Hash:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_LockRows:
		case T_Limit:
		case T_ModifyTable:
		case T_Append:
		case T_MergeAppend:
		case T_RecursiveUnion:
		case T_Motion:
		case T_ShareInputScan:
		case T_Sequence:
			return false;
		default:
			break;
	}
	return true;
}

/*
 * plan_pushdown_tlist
 *
 * If the given Plan node does projection, the same node is returned after
 * replacing its targetlist with the given targetlist.
 *
 * Otherwise, returns a Result node with the given targetlist, inserted atop
 * the given plan.
 */
Plan *
plan_pushdown_tlist(PlannerInfo *root, Plan *plan, List *tlist)
{
	bool		need_result;

	if (!is_projection_capable_plan(plan) &&
		!tlist_same_exprs(tlist, plan->targetlist))
	{
		need_result = true;
	}
	else
		need_result = false;

	if (!need_result)
	{
		/* Install the new targetlist. */
		plan->targetlist = tlist;
	}
	else
	{
		Plan	   *subplan = plan;

		/* Insert a Result node to evaluate the targetlist. */
		plan = (Plan *) inject_projection_plan(subplan, tlist);
	}
	return plan;
}	/* plan_pushdown_tlist */

static TargetEntry *
find_junk_tle(List *targetList, const char *junkAttrName)
{
	ListCell	*lct;

	foreach(lct, targetList)
	{
		TargetEntry	*tle = (TargetEntry*) lfirst(lct);

		if (!tle->resjunk)
			continue;

		if (!tle->resname)
			continue;

		if (strcmp(tle->resname, junkAttrName) == 0)
			return tle;
	}
	return NULL;
}

/*
 * cdbpathtoplan_create_motion_plan
 */
static Motion *
cdbpathtoplan_create_motion_plan(PlannerInfo *root,
								 CdbMotionPath *path,
								 Plan *subplan)
{
	Motion	   *motion = NULL;
	Path	   *subpath = path->subpath;
	int			numsegments;

	if (CdbPathLocus_IsOuterQuery(path->path.locus) ||
		CdbPathLocus_IsEntry(path->path.locus))
		numsegments = 1;  /* dummy numsegments */
	else
		numsegments = CdbPathLocus_NumSegments(path->path.locus);

	if (path->is_explicit_motion)
	{
		TargetEntry *segmentid_tle;

		Assert(CdbPathLocus_IsPartitioned(path->path.locus));

		/*
		 * The junk columns in the subplan need to be labeled as such, otherwise
		 * we won't find the "gp_segment_id" column.
		 *
		 * The target list of a SplitUpdate is correctly labeled already. It has
		 * different layout than normal ModifyTable inputs, because it contains
		 * the DMLActionExpr column, so we cannot apply the
		 * labeling here even if we wanted.
		 */
		if (!IsA(subplan, SplitUpdate))
			apply_tlist_labeling(subplan->targetlist, root->processed_tlist);

		segmentid_tle = find_junk_tle(subplan->targetlist, "gp_segment_id");
		if (!segmentid_tle)
			elog(ERROR, "could not find gp_segment_id in subplan's targetlist");
		motion = (Motion *) make_explicit_motion(root, subplan, segmentid_tle->resno,
												 numsegments);
	}
	else if (path->policy)
	{
		List	   *hashExprs = NIL;
		List	   *hashOpfamilies = NIL;

		for (int i = 0; i < path->policy->nattrs; ++i)
		{
			AttrNumber	attno = path->policy->attrs[i];
			Expr	   *expr;
			Oid			opfamily = get_opclass_family(path->policy->opclasses[i]);

			expr = list_nth(subpath->pathtarget->exprs, attno - 1);

			hashExprs = lappend(hashExprs, expr);
			hashOpfamilies = lappend_oid(hashOpfamilies, opfamily);
		}

		/**
		 * If there are subplans in the hashExpr, push it down to lower level.
		 */
		if (contain_subplans((Node *) hashExprs))
		{
			/* make a Result node to do the projection if necessary */
			if (!is_projection_capable_plan(subplan))
			{
				List	   *tlist = copyObject(subplan->targetlist);

				subplan = (Plan *) make_result(tlist, NULL, subplan);
			}
			subplan->targetlist = add_to_flat_tlist_junk(subplan->targetlist,
														 hashExprs,
														 true /* resjunk */);
		}

		motion = make_hashed_motion(subplan,
									hashExprs,
									hashOpfamilies,
									numsegments);
	}
	else if (CdbPathLocus_IsOuterQuery(path->path.locus))
	{
		motion = make_union_motion(subplan, numsegments);
		motion->motionType = MOTIONTYPE_OUTER_QUERY;
	}
	/* Send all tuples to a single process? */
	else if (CdbPathLocus_IsBottleneck(path->path.locus))
	{
		if (path->path.pathkeys)
		{
			Plan	   *prep;
			int			numSortCols;
			AttrNumber *sortColIdx;
			Oid		   *sortOperators;
			Oid		   *collations;
			bool		*nullsFirst;

			/*
			 * Build sort key info to define our Merge Receive keys.
			 */
			prep = prepare_sort_from_pathkeys(subplan,
											  path->path.pathkeys,
											  subpath->parent->relids,
											  NULL,
											  false,
											  &numSortCols,
											  &sortColIdx,
											  &sortOperators,
											  &collations,
											  &nullsFirst,
											  true /* add_keys_to_targetlist */);

			if (prep)
			{
				/*
				 * Create a Merge Receive to preserve ordering.
				 *
				 * prepare_sort_from_pathkeys() might return a Result node, if
				 * one would needs to be inserted above the Sort. We don't
				 * create an actual Sort node here, the input is already
				 * ordered, but use the Result node, if any, as the input to
				 * the Motion node. (I'm not sure if that is possible with
				 * Gather Motion nodes. Since the input is already ordered,
				 * presumably the target list already contains the expressions
				 * for the key columns. But better safe than sorry.)
				 */
				subplan = prep;
				motion = make_sorted_union_motion(root, subplan, numSortCols, sortColIdx, sortOperators, collations,
												  nullsFirst, numsegments);
			}
			else
			{
				/* Degenerate ordering... build unordered Union Receive */
				motion = make_union_motion(subplan, numsegments);
			}
		}

		/* Unordered Union Receive */
		else
		{
			motion = make_union_motion(subplan, numsegments);
		}
	}

	/* Send all of the tuples to all of the QEs in gang above... */
	else if (CdbPathLocus_IsReplicated(path->path.locus))
		motion = make_broadcast_motion(subplan, numsegments);

	/* Hashed redistribution to all QEs in gang above... */
	else if (CdbPathLocus_IsHashed(path->path.locus) ||
			 CdbPathLocus_IsHashedOJ(path->path.locus))
	{
		List	   *hashExprs;
		List	   *hashOpfamilies;

		cdbpathlocus_get_distkey_exprs(path->path.locus,
									   path->path.parent->relids,
									   subplan->targetlist,
									   &hashExprs, &hashOpfamilies);
		if (!hashExprs)
			elog(ERROR, "could not find hash distribution key expressions in target list");

		/**
         * If there are subplans in the hashExpr, push it down to lower level.
         */
		if (contain_subplans((Node *) hashExprs))
		{
			/* make a Result node to do the projection if necessary */
			if (!is_projection_capable_plan(subplan))
			{
				List	   *tlist = copyObject(subplan->targetlist);

				subplan = (Plan *) make_result(tlist, NULL, subplan);
			}
			subplan->targetlist = add_to_flat_tlist_junk(subplan->targetlist,
														 hashExprs,
														 true /* resjunk */);
        }
        motion = make_hashed_motion(subplan,
									hashExprs,
									hashOpfamilies,
									numsegments);
    }
	/* Hashed redistribution to all QEs in gang above... */
	else if (CdbPathLocus_IsStrewn(path->path.locus))
	{
		motion = make_hashed_motion(subplan,
									NIL,
									NIL,
									numsegments);
	}
	else
		elog(ERROR, "unexpected target locus type %d for Motion node", path->path.locus.locustype);

	/* Remember that this subtree contains a Motion */
	root->numMotions++;

	return motion;
}								/* cdbpathtoplan_create_motion_plan */
