#include "moar.h"

#if defined(MVM_JIT_DEBUG) && (MVM_JIT_DEBUG & MVM_JIT_DEBUG_OPTIMIZER) != 0
#define _DEBUG(fmt, ...) do { MVM_jit_log(tc, fmt "%s", __VA_ARGS__, "\n"); } while(0)
#else
#define _DEBUG(fmt, ...) do {} while(0)
#endif

struct NodeRef {
    MVMint32 parent;
    MVMint32 ptr;
    MVMint32 next;
};

struct NodeInfo {
    MVMint32 refs;
    MVMint32 ref_cnt;
};

struct Optimizer {
    MVM_VECTOR_DECL(struct NodeRef, refs);
    MVM_VECTOR_DECL(struct NodeInfo, info);
    MVM_VECTOR_DECL(MVMint32, replacements);
    MVMint32 replacement_cnt;
};

static void optimize_preorder(MVMThreadContext *tc, MVMJitTreeTraverser *traverser,
                              MVMJitExprTree *tree, MVMint32 node) {
    /* i can imagine that there might be interesting optimizations we could apply here */

}

static void replace_node(MVMThreadContext *tc, MVMJitTreeTraverser *traverser,
                         MVMJitExprTree *tree, MVMint32 node, MVMint32 replacement) {
    MVMint32 *c;
    struct Optimizer *o = traverser->data;
    /* double pointer iteration to update all children */
    _DEBUG("Replaced node %d with %d", node, replacement);

    MVM_VECTOR_ENSURE_SIZE(traverser->visits, replacement);
    MVM_VECTOR_ENSURE_SIZE(o->info, replacement);
    MVM_VECTOR_ENSURE_SIZE(o->replacements, replacement);

    for (c = &o->info[node].refs; *c > 0; c = &o->refs[*c].next) {
        tree->nodes[o->refs[*c].ptr] = replacement;
    }

    /* append existing to list */
    if (o->info[replacement].refs > 0) {
        *c = o->info[replacement].refs;
    }
    o->info[replacement].refs = o->info[node].refs;
    o->info[replacement].ref_cnt += o->info[node].ref_cnt;

    o->replacements[node] = replacement;
    o->replacement_cnt++;

    /* NB - we only need this for the op_info which is looked up everywhere, and
     * we only need that for the nargs < 0 check, which I want to get rid of and
     * replace with a bitmap - and that implies that I might be able to make the
     * info structure transient. */
    MVM_VECTOR_ENSURE_SIZE(tree->info, replacement);
    tree->info[replacement].op_info = MVM_jit_expr_op_info(tc, tree->nodes[replacement]);
}

static void optimize_child(MVMThreadContext *tc, MVMJitTreeTraverser *traverser,
                           MVMJitExprTree *tree, MVMint32 node, MVMint32 child) {
    /* add reference from parent to child, replace child with reference if possible */
    MVMint32 first_child = tree->info[node].op_info->nchild < 0 ? node + 2 : node + 1;
    MVMint32 child_node = tree->nodes[first_child+child];
    struct Optimizer *o = traverser->data;

    /* double referenced LOAD replacement */
     if (tree->nodes[child_node] == MVM_JIT_LOAD &&
         o->info[child_node].ref_cnt > 1) {
         _DEBUG("optimizing multiple (ref_cnt=%d) LOAD (%d) to COPY", o->info[child_node].ref_cnt, child_node);
         MVMint32 replacement = MVM_jit_expr_apply_template_adhoc(tc, tree, "..", MVM_JIT_COPY, child_node);
         replace_node(tc, traverser, tree, child_node, replacement);
    }


    if (o->replacements[child_node] > 0) {
        _DEBUG("Parent node %d assigning replacement node (%d -> %d)",
               node, child_node, o->replacements[child_node]);
        child_node = o->replacements[child_node];
        tree->nodes[first_child+child] = child_node;
        o->replacement_cnt++;
    }

    /* add this parent node as a reference */
    MVM_VECTOR_ENSURE_SPACE(o->refs, 1);
    {
        MVMint32 r = o->refs_num++;

        o->refs[r].next = o->info[child_node].refs;
        o->refs[r].parent = node;
        o->refs[r].ptr    = first_child + child;

        o->info[child_node].refs = r;
        o->info[child_node].ref_cnt++;
    }

}




static void optimize_postorder(MVMThreadContext *tc, MVMJitTreeTraverser *traverser,
                               MVMJitExprTree *tree, MVMint32 node) {
    /* after postorder, we will not revisit the node, so it's time to replace it */
    struct Optimizer *o = traverser->data;
    MVMint32 replacement = -1;

    switch(tree->nodes[node]) {
    case MVM_JIT_IDX:
    {
        MVMint32 base = tree->nodes[node+1];
        MVMint32 idx  = tree->nodes[node+2];
        MVMint32 scale = tree->nodes[node+3];
        if (tree->nodes[idx] == MVM_JIT_CONST) {
            MVMint32 ofs = tree->nodes[idx+1] * scale;
            _DEBUG("Const idx (node=%d, base=%d, idx=%d, scale=%d, ofs=%d)", node, base, idx, scale, ofs);
            /* insert addr (base, $ofs) */
            replacement = MVM_jit_expr_apply_template_adhoc(tc, tree, "...", MVM_JIT_ADDR, base, ofs);
        }
        break;
    }
    }

    if (replacement > 0) {
        replace_node(tc, traverser, tree, node, replacement);
    }
}


void MVM_jit_expr_tree_optimize(MVMThreadContext *tc, MVMJitGraph *jg, MVMJitExprTree *tree) {
    MVMJitTreeTraverser t;
    struct Optimizer o;
    MVMint32 i;

    /* will nearly always be enough */
    MVM_VECTOR_INIT(o.refs, tree->nodes_num);
    MVM_VECTOR_INIT(o.info, tree->nodes_num * 2);
    MVM_VECTOR_INIT(o.replacements, tree->nodes_num * 2);
    o.replacement_cnt = 0;

    /* Weasly trick: we increment the o->refs_num by one, so that we never
     * allocate zero, so that a zero *refs is the same as 'nothing', so that we
     * don't have to initialize all the refs with -1 in order to indicate a
     * terminator */
    o.refs_num++;

    t.preorder  = optimize_preorder;
    t.inorder   = optimize_child;
    t.postorder = optimize_postorder;
    t.data      = &o;
    t.policy    = MVM_JIT_TRAVERSER_ONCE;
    MVM_jit_expr_tree_traverse(tc, tree, &t);

    MVM_VECTOR_DESTROY(o.refs);
    MVM_VECTOR_DESTROY(o.info);
    MVM_VECTOR_DESTROY(o.replacements);
}
