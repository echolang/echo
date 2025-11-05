#ifndef ASTTYPECHECKER_H
#define ASTTYPECHECKER_H

#pragma once

#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTBundle.h"
#include "AST/ASTCodeRef.h"
#include "AST/ASTNullability.h"
#include "Compiler/CompilerOptions.h"

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
        // the options come in because one diagnostic depends on them: `mem::live_allocations()` reads a
        // counter only --track-allocations maintains, and a builtin that answers 0 where nothing counted
        // is the one wrong answer a caller cannot tell from the right one. it is refused here rather than
        // in codegen because codegen has only InternalCompilerException, which carries no source location
        //
        // defaulted, so a test or a tool that only wants the type checking need not have an opinion
        explicit TypeChecker(Bundle &bundle, Compiler::CompilerOptions options = {});

        void run();

        void visitFunctionDecl(FunctionDeclNode &node) override;
        void visit_type_decl(TypeDeclNode &node) override;
        void visitMemberAccess(MemberAccessNode &node) override;
        void visit_instanceof_expr(InstanceOfExprNode &node) override;
        void visit_strong_expr(StrongExprNode &node) override;

        // the three nullability forms, re-asked here for the reason every type question in this
        // compiler is asked here: inside a template the operand is a bare `T`, which
        // AST::is_certainly_present deliberately answers "later" for, and the parser is the only place
        // that ever asked. see todo/B27
        void visit_guard(GuardNode &node) override;
        void visit_null_coalesce(NullCoalesceExprNode &node) override;
        void visit_optional_chain(OptionalChainExprNode &node) override;

        void visitFunctionCallExpr(FunctionCallExprNode &node) override;
        void visit_indirect_call_expr(IndirectCallExprNode &node) override;
        void visit_closure_expr(ClosureExprNode &node) override;
        void visitVarDecl(VarDeclNode &node) override;
        void visit_assign(AssignNode &node) override;
        void visitTypeCast(TypeCastNode &node) override;
        void visitBinaryExpr(BinaryExprNode &node) override;
        void visitUnaryExpr(UnaryExprNode &node) override;
        void visit_addr_of_expr(AddrOfExprNode &node) override;
        void visitReturn(ReturnNode &node) override;

    private:
        Bundle &_bundle;
        Collector &_collector;

        // what the invocation asked for, read through its predicates rather than compared - the same
        // rule every other reader of it follows
        Compiler::CompilerOptions _options;

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

        // validates every `: SomeInterface` this declaration wrote, reporting the first requirement each
        // one leaves unanswered. **called ahead of the generic early-return** in visit_type_decl, since a
        // generic implementor is checked on its template - see the note there
        void check_conformances(TypeDeclNode &node);

        // reports a declaration that has no body and no marker saying where its implementation comes
        // from. **called ahead of the generic early-return** in visitFunctionDecl, since a bodyless
        // template is exactly one of the cases - see the note there
        void check_has_implementation(FunctionDeclNode &node);

        // does this value conform but still fail to be *storable* as `to`? reports and answers true when
        // so. see the implementation for why it runs ahead of the ordinary conversion check
        bool check_interface_erasure(const ValueType &to, const ExprNode &value, const TokenReference &at);

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

        // the single "does this argument reach this parameter" rule, shared by a direct call - which
        // takes its parameters off `decl->args` - and an indirect one, which takes them off the callee's
        // signature. only the three things a declaration would have supplied are passed in: the number
        // a reader counts to, the name to blame, and where to point
        //
        // both halves matter and neither is derivable from the other: a `null` written into a
        // non-nullable parameter is refused before the fit is scored, because the promise the
        // declaration site makes only holds if the call site keeps it - and reading through it was a
        // segfault, not a mismatch
        void check_call_argument(
            ExprNode *argument,
            const ValueType &param_type,
            size_t arg_number,
            const std::string &callee_name,
            const TokenReference &at
        );

        // `die`'s and `assert`'s message has to be a string literal, because codegen folds it into a
        // constant along with the call site's source location. reported here rather than in the
        // parser: the declaration is what makes the call legal, and codegen has to be able to trust
        // the shape by the time it reads it. a no-op for every other call
        void check_abort_message(FunctionCallExprNode &node);
        void check_ref_count_argument(FunctionCallExprNode &node);
        void check_dprint_argument(FunctionCallExprNode &node);

        // `mem::take<T>` ends its source's claim on a value without writing anything back, so the two
        // ways of getting it wrong are a source that was never a place and a source something *else*
        // already accounts for. refused here, where the call has a token to point at
        void check_take_argument(FunctionCallExprNode &node);

        // the one builtin whose *availability* is a question rather than its arguments: without
        // --track-allocations there is no counter for `mem::live_allocations()` to read
        void check_allocation_tracking(FunctionCallExprNode &node);

        // the shared half of the three arms above: refuse an operand that can never be absent, worded
        // by AST::certainly_present_refusal. the three differ only in which expression they hand it
        // and where they point, so the rule is written once
        void check_optional_operand(
            OptionalForm form, const ExprNode *operand, const TokenReference &at);

        // rejects an assignment that reaches const storage. split out of visit_assign because it
        // asks a different question than the conversion check next to it: not "does the value
        // fit" but "may this storage be written at all"
        void check_const_target(AssignNode &node);

        // rejects a call whose receiver is const and whose callee is not, worded by
        // AST::const_receiver_refusal. **true when it reported**, so the argument loop drops argument 0
        // rather than adding a second, vaguer message about the same value - the contract
        // check_interface_erasure already has, and for the same reason: the reader needs the method's
        // name and the `const function` spelling, not the name of a conversion
        bool check_receiver_const(FunctionCallExprNode &node);
    };
};

#endif
