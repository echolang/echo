#ifndef EXPRCODEGEN_H
#define EXPRCODEGEN_H

#pragma once

#include "AST/ASTBuiltin.h"
#include "AST/ASTValueType.h"

#include <vector>

namespace llvm
{
    class Value;
    class Function;
};

namespace AST
{
    class FunctionDeclNode;
    class TypeCastNode;
    class VarRefNode;
    class LiteralFloatExprNode;
    class LiteralIntExprNode;
    class LiteralBoolExprNode;
    class LiteralStringExprNode;
    class BinaryExprNode;
    class UnaryExprNode;
    class FunctionCallExprNode;
    class ClosureExprNode;
    class IndirectCallExprNode;
    class AddrOfExprNode;
    class StrongExprNode;
    class NullCoalesceExprNode;
    class OptionalChainExprNode;
    class ChainBaseNode;
    class DerefExprNode;
    class TemporaryBindExprNode;
    class IndexExprNode;
    class NullNode;
    class OperatorNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;
    struct LValue;

    // lowers expression nodes (literals, casts, arithmetic/logical operators, calls, variable and
    // pointer references) to llvm values, leaving the produced value on the context value stack
    class ExprCodegen
    {
    public:
        ExprCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void gen_type_cast(AST::TypeCastNode &node);
        void gen_var_ref(AST::VarRefNode &node);
        void gen_literal_float(AST::LiteralFloatExprNode &node);
        void gen_literal_int(AST::LiteralIntExprNode &node);
        void gen_literal_bool(AST::LiteralBoolExprNode &node);
        void gen_literal_string(AST::LiteralStringExprNode &node);
        void gen_binary_expr(AST::BinaryExprNode &node);
        void gen_unary_expr(AST::UnaryExprNode &node);
        void gen_function_call(AST::FunctionCallExprNode &node);

        // `{ fn, env }` from a closure literal: the body's address, and the environment the capture pass
        // put there - a null pointer when nothing is captured
        void gen_closure_expr(AST::ClosureExprNode &node);

        // a call through a callable value. the `fn` slot is extracted and called with the `env` slot
        // prepended, which is why every callable target takes the environment as parameter 0
        void gen_indirect_call(AST::IndirectCallExprNode &node);

        // answers a `#[builtin: ...]` call in the compiler instead of emitting a call - a builtin
        // has no symbol at all. dispatches on AST::BuiltinKind to one of the three below
        void gen_builtin_call(AST::FunctionCallExprNode &node);
        void gen_addr_of(AST::AddrOfExprNode &node);

        // `strong($w)`. the branch-and-phi lives in ClassCodegen beside the counts it reads, so this arm
        // only evaluates the operand and routes - the same split gen_ref_count_builtin makes
        void gen_strong_expr(AST::StrongExprNode &node);

        // `A ?? B`: evaluate A, test it, and take B only when it is absent. a branch and a phi rather than
        // a select, because **B must not be evaluated on the present path** - it may be a call
        void gen_null_coalesce(AST::NullCoalesceExprNode &node);

        // `A?->b`: evaluate A, test it, and run the continuation only when it is there. the same shape as
        // `??` with the arms the other way round, and the absent arm supplying the destination's null
        void gen_optional_chain(AST::OptionalChainExprNode &node);

        // the marker standing for a chain's unwrapped base. pushes the value the enclosing chain stashed -
        // it evaluates nothing, because the base was already evaluated once, before the branch
        void gen_chain_base(AST::ChainBaseNode &node);

        void gen_deref(AST::DerefExprNode &node);

        // binds the temporaries, evaluates the body, runs the teardown, and hands back the body's
        // value - the four steps AST::TemporaryBindExprNode is, in that order
        void gen_temporary_bind(AST::TemporaryBindExprNode &node);
        void gen_index(AST::IndexExprNode &node);
        void gen_null(AST::NullNode &node);
        void gen_operator(AST::OperatorNode &node);

    private:
        CodegenContext &_ctx;

        // the llvm::Function a declaration was emitted as, searching this unit first and then every
        // other one - a declaration is emitted into exactly one unit, and a call may cross that line.
        // null when nothing was emitted for it; the caller phrases the diagnostic
        llvm::Function *find_llvm_function(const AST::FunctionDeclNode *decl);

        // a call whose declaration is an interface **requirement**: the callee is loaded out of the
        // receiver's own vtable rather than looked up in the function table, because a requirement has no
        // symbol - the implementors have the bodies, under their own names
        //
        // it needed no new node and no new resolution rule. the requirement answers four of the five
        // things `decl` is at a direct call - the return type, the parameter list, the coercion walk and
        // the diagnostic name - and only the *symbol* differs, which is exactly this one arm. what makes
        // the receiver free is `%eco.iface`'s field order: a class method's `$this` is the address of a
        // slot holding a handle, and the address of the object field is precisely that
        void gen_virtual_call(AST::FunctionCallExprNode &node);

        // **a question about a type, folded to a constant**: `size_of` / `align_of`, and the two
        // ownership predicates `is_trivially_copyable` / `needs_destruction`. all four are asked of the
        // instance's single type argument, which the monomorphizer stamped on when it resolved
        // `size_of<int32>()` from `size_of<T>()`
        //
        // one arm for the layout pair and the taxonomy pair, even though one reads a DataLayout and the
        // other reads the AST: what they share is the part that is easy to get wrong, which is the
        // *concreteness* guard on that type argument. every fold in the language that answers "no" for an
        // unsettled type parameter belongs behind one check, not behind two written at different times
        //
        // `kind` is passed rather than re-derived: gen_builtin_call has already made the routing
        // decision, and taking it as a parameter says in the signature that only those kinds arrive
        void gen_type_query_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind);

        // the address of the place argument 0 names, for the two builtins that read or write raw storage.
        //
        // one invariant - "argument 0 is the address of a place" - and therefore one guard: `take` and
        // `init` are a load and a store *through the same thing*, and their preambles were two copies of
        // the arity check, the pointer check and the message that names it. What is left in each arm is
        // its one instruction.
        //
        // the shape is AST::TypeChecker::check_raw_storage_argument's, not this one's - what is checked
        // here is the invariant a settled call already carries. `name` is what the refusals say, `arity`
        // how many arguments the builtin declares
        LValue gen_raw_place(AST::FunctionCallExprNode &node, const char *name, size_t arity);

        // `take`: the value at a place, read out with no copy inserted and nothing written back
        //
        // one load, and every consequence follows from this being a *call* rather than a place - see
        // AST::BuiltinKind::t_take. no `kind` parameter, because it is the only one of its family
        void gen_take_builtin(AST::FunctionCallExprNode &node);

        // `init`: the value stored into a place that held nothing, with no teardown of what was there
        //
        // one store, and the mirror of the arm above in every respect - see AST::BuiltinKind::t_init.
        // pushes nothing: it returns void, so it is a statement, as `free_bytes` is
        void gen_init_builtin(AST::FunctionCallExprNode &node);

        // `die`: stop, unconditionally
        void gen_die_builtin(AST::FunctionCallExprNode &node);

        // `assert`: stop when the condition is false, and in a release build emit nothing at all -
        // not even the condition, which is what CompilerOptions::assertions_enabled decides
        void gen_assert_builtin(AST::FunctionCallExprNode &node);

        // `ref_count<T>(T& $handle)` and `weak_count<T>(T& $handle)`. the builtins that are both generic
        // and take an argument, so they share neither family's shape - see AST::BuiltinKind::t_ref_count.
        // one arm for both because they differ only in which header word they load
        void gen_ref_count_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind);

        // `dprint<T>(T& $value)`. the same generic-plus-borrow shape as the two counts above, but the only
        // builtin that *emits* rather than folding - and the only one whose lowering creates basic blocks,
        // which is why everything after the address is DebugPrintCodegen's. this arm reads the subject
        // type and the slot, and routes
        void gen_dprint_builtin(AST::FunctionCallExprNode &node);

        // `alloc_bytes` / `realloc_bytes` / `free_bytes`: the raw allocator, routed to the one place that
        // hands out heap memory. concrete, so unlike every other builtin here there is no type argument to
        // read - the arguments are ordinary values and the only work is evaluating them
        //
        // `kind` is passed for gen_type_query_builtin's reason, and it carries more here: it is also what
        // says how many arguments to expect, so the three cannot disagree about their own arities
        void gen_raw_memory_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind);

        // `live_allocations()`. concrete, argument-less, and the only builtin that reads state the *running
        // program* maintains rather than a fact the compiler knows - so it is the only one that folds to
        // nothing and loads instead
        void gen_live_allocations_builtin(AST::FunctionCallExprNode &node);

        // `process_argc` / `process_argv` / `process_envp`. concrete, argument-less, and reading running
        // state like the one above - except the state is what the entry point was handed rather than
        // something the program accumulated, so there is nothing to gate on and nothing to refuse
        //
        // `kind` is passed for gen_type_query_builtin's reason: one arm, three globals, and the routing
        // decision made once
        void gen_process_query_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind);

        // `exit(int32 $code)`. pushes no value and terminates the block, the shape `die` has - the
        // difference is that the code is a runtime value rather than a compile-time message, so this is
        // the one stop site whose argument has to be evaluated
        void gen_exit_builtin(AST::FunctionCallExprNode &node);

        // `echo` of a string or a string view: a length-counted write(2) rather than a printf, because
        // the bytes are not NUL-terminated in general. the codegen half of the rule
        // TypeChecker::visitFunctionCallExpr admits
        void gen_echo_string(llvm::Value *value, const AST::ValueType &type);

        // stops when `address` is null. emitted where a nullable pointer is narrowed to a
        // borrow, which is the one conversion that asserts rather than merely reinterprets
        // debug builds only - in release the narrowing is unchecked, as the doc says
        void gen_null_assert(llvm::Value *address);
    };
};

#endif
