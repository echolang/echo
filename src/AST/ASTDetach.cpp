#include "AST/ASTDetach.h"

#include "AST/ASTBundle.h"
#include "AST/ASTNode.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ExprNode.h"

#include <unordered_set>

namespace
{
    // every node in a subtree.
    //
    // **total by construction**, which is the only reason its answer may be handed to
    // NodeCollection::forget: it overrides the two funnels every owned edge in AST::RecursiveVisitor passes
    // through - the value one and the statement one - so a node kind added later is reached without this
    // file being edited. a hand-rolled switch here would under-collect silently
    class DetachedNodes : public AST::RecursiveVisitor
    {
    public:
        DetachedNodes(std::unordered_set<const AST::Node *> &into) : gone(into) {}

        std::unordered_set<const AST::Node *> &gone;

        // the root arrives through no edge of its own, so it is added by hand
        void collect(AST::Node &root)
        {
            gone.insert(&root);
            root.accept(*this);
        }

    protected:
        AST::ExprNode *rewrite_value_edge(AST::ExprNode *expr) override
        {
            if (expr != nullptr) {
                gone.insert(expr);
            }

            return AST::RecursiveVisitor::rewrite_value_edge(expr);
        }

        void statement_edge(AST::Node *node) override
        {
            if (node != nullptr) {
                gone.insert(node);
            }

            AST::RecursiveVisitor::statement_edge(node);
        }
    };
}

void AST::collect_subtree(AST::Node &root, std::unordered_set<const AST::Node *> &gone)
{
    DetachedNodes detached(gone);
    detached.collect(root);
}

void AST::DetachBatch::flush(AST::Bundle &bundle)
{
    bundle.forget_nodes(_gone);
    _gone.clear();
}

void AST::forget_subtree(AST::Bundle &bundle, AST::Node &root)
{
    std::unordered_set<const AST::Node *> gone;
    AST::collect_subtree(root, gone);

    bundle.forget_nodes(gone);
}
