#include "AST/ASTMutation.h"

#include "AST/ASTPlaceExpr.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/ForeachNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ReturnNode.h"
#include "AST/VarDeclNode.h"

namespace
{
    class WriteFinder : public AST::RecursiveVisitor
    {
    public:
        WriteFinder(const AST::VarDeclNode &decl) : _decl(&decl) {}

        bool found() const { return _written; }

        void visit_assign(AST::AssignNode &node) override
        {
            // the target's *root*, so `$el = v`, `$el->x = v`, `$el[$i] = v` and `$el:$ = v` are one
            // question - AST::place_root_of already walks all four shapes
            note(node.target);
            AST::RecursiveVisitor::visit_assign(node);
        }

        void visit_addr_of_expr(AST::AddrOfExprNode &node) override
        {
            // `&$el`, and every method receiver: the parser addresses one as `AddrOf(...)`, so
            // `$el->m()` is caught here rather than needing an arm of its own
            note(node.operand);
            AST::RecursiveVisitor::visit_addr_of_expr(node);
        }

        void visit_move_expr(AST::MoveExprNode &node) override
        {
            note(node.operand);
            AST::RecursiveVisitor::visit_move_expr(node);
        }

        void visitFunctionCallExpr(AST::FunctionCallExprNode &node) override
        {
            // `echo` is the exception, through the one owner of "is this the decl-less print builtin".
            // it neither writes nor borrows
            if (!AST::is_print_call(node)) {
                for (auto *arg : node.arguments) {
                    note(arg);
                }
            }

            AST::RecursiveVisitor::visitFunctionCallExpr(node);
        }

        void visit_indirect_call_expr(AST::IndirectCallExprNode &node) override
        {
            for (auto *arg : node.arguments) {
                note(arg);
            }

            AST::RecursiveVisitor::visit_indirect_call_expr(node);
        }

        void visitReturn(AST::ReturnNode &node) override
        {
            // returning it reads it *out*, which from a `const V&` is a copy - and if V owns something
            // with no copy constructor that is a diagnostic the value binding would not have earned
            note(node.expr);
            AST::RecursiveVisitor::visitReturn(node);
        }

        void visit_closure_expr(AST::ClosureExprNode &node) override
        {
            // a captured `const V&` dangles the moment the iteration steps
            for (auto *value : node.captured_values) {
                note(value);
            }

            AST::RecursiveVisitor::visit_closure_expr(node);
        }

        void visit_index_expr(AST::IndexExprNode &node) override
        {
            // a bracket whose base has already moved into its element_call cannot be seen through, so
            // it is answered *written* rather than guessed at. unreachable while `$el` is untyped -
            // resolve_index defers on an undetermined base - and the conservative answer if it is not
            if (node.base == nullptr) {
                _written = true;
            }

            AST::RecursiveVisitor::visit_index_expr(node);
        }

        void visit_foreach(AST::ForeachNode &node) override
        {
            // a nested loop over it takes its address, exactly as case (a) of the lowering does
            note(node.source);
            AST::RecursiveVisitor::visit_foreach(node);
        }

    private:
        void note(AST::ExprNode *expr)
        {
            if (expr != nullptr && AST::place_root_of(expr) == _decl) {
                _written = true;
            }
        }

        const AST::VarDeclNode *_decl = nullptr;
        bool _written = false;
    };
}

bool AST::is_never_written(const AST::VarDeclNode &decl, AST::Node &subtree)
{
    WriteFinder finder(decl);
    subtree.accept(finder);

    return !finder.found();
}
