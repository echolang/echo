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

    // a semantic-analysis pass that runs after monomorphization and before codegen. it walks the
    // concrete AST resolving member accesses and call arguments, and records located issues on the
    // collector (never throws, so main() keeps gating on has_critical_issues()). this moves the
    // "missing member / wrong argument" diagnostics off the lazy codegen throw sites, where the
    // error surfaced maximally far from its cause, to a point that still has full source context.
    //
    // generic function templates are skipped: their bodies mention the template's type parameters
    // and are only meaningful once the monomorphizer has cloned them into concrete instances (which
    // this pass does check).
    class TypeChecker : public RecursiveVisitor
    {
    public:
        explicit TypeChecker(Bundle &bundle);

        void run();

        void visitFunctionDecl(FunctionDeclNode &node) override;
        void visitStructDecl(StructDeclNode &node) override;
        void visitMemberAccess(MemberAccessNode &node) override;
        void visitFunctionCallExpr(FunctionCallExprNode &node) override;
        void visitVarDecl(VarDeclNode &node) override;
        void visitTypeCast(TypeCastNode &node) override;
        void visitBinaryExpr(BinaryExprNode &node) override;

    private:
        Bundle &_bundle;
        Collector &_collector;

        // the module/file currently being walked, used to build located code references.
        Module *_current_module = nullptr;
        File *_current_file = nullptr;

        // the token of the statement currently being walked (call name, declared variable, ...),
        // used to locate diagnostics for nodes that carry no token of their own (e.g. the implicit
        // casts the parser/monomorphizer inserts around mismatched arguments).
        const TokenReference *_context_token = nullptr;

        CodeRef code_ref_for(const TokenReference &token);
    };
}

#endif
