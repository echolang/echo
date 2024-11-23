#ifndef ASTTYPECHECKER_H
#define ASTTYPECHECKER_H

#pragma once

#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTBundle.h"
#include "AST/ASTCodeRef.h"

namespace AST
{
    class Module;
    class File;
    class BinaryExprNode;
    class ExprNode;

    // a semantic-analysis pass that runs after monomorphization and before codegen. it walks the
    // concrete AST resolving member accesses and call arguments, and records located issues on the
    // collector (never throws, so main() keeps gating on has_critical_issues()). this moves the
    // "missing member / wrong argument" diagnostics off the lazy codegen throw sites, where the
    // error surfaced maximally far from its cause, to a point that still has full source context
    //
    // generic function templates are skipped: their bodies mention the template's type parameters
    // and are only meaningful once the monomorphizer has cloned them into concrete instances (which
    // this pass does check)
    class TypeChecker : public RecursiveVisitor
    {
    public:
        explicit TypeChecker(Bundle &bundle);

        void run();

        void visitFunctionDecl(FunctionDeclNode &node) override;
        void visit_type_decl(TypeDeclNode &node) override;
        void visitMemberAccess(MemberAccessNode &node) override;
        void visit_instanceof_expr(InstanceOfExprNode &node) override;
        void visitFunctionCallExpr(FunctionCallExprNode &node) override;
        void visitVarDecl(VarDeclNode &node) override;
        void visit_assign(AssignNode &node) override;
        void visitTypeCast(TypeCastNode &node) override;
        void visitBinaryExpr(BinaryExprNode &node) override;
        void visitReturn(ReturnNode &node) override;

    private:
        Bundle &_bundle;
        Collector &_collector;

        // the function whose body is being walked, so a return knows what it has to fit and
        // which variables are the caller's. null at file scope
        FunctionDeclNode *_current_function = nullptr;

        // the module/file currently being walked, used to build located code references
        Module *_current_module = nullptr;
        File *_current_file = nullptr;

        // the token of the statement currently being walked (call name, declared variable, ...),
        // used to locate diagnostics for nodes that carry no token of their own (e.g. the implicit
        // casts the parser/monomorphizer inserts around mismatched arguments)
        const TokenReference *_context_token = nullptr;

        CodeRef code_ref_for(const TokenReference &token);

        // the destinations that a value can be written into. only the phrasing of the diagnostic
        // differs between them - the rule that decides whether the value fits does not
        enum class Destination
        {
            t_declaration,
            t_assignment,
            t_return,
        };

        // the single "does this value fit this destination" rule, shared by a declaration's
        // initializer, an assignment target and a return. it used to be written out at each of the
        // three, and had already drifted: the return copy gated on "a pointer is involved" and so
        // never checked a struct return at all
        void check_destination_fits(Destination dest, const ValueType &to, const ExprNode &value, const TokenReference &at);

        // rejects an assignment that reaches const storage. split out of visit_assign because it
        // asks a different question than the conversion check next to it: not "does the value
        // fit" but "may this storage be written at all"
        void check_const_target(AssignNode &node);
    };
};

#endif
