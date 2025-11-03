#ifndef ASTFUNCTIONEMISSION_H
#define ASTFUNCTIONEMISSION_H

#pragma once

namespace AST
{
    class FunctionDeclNode;

    // how a function declaration becomes a symbol: whether it has one at all, whether *this* compiler
    // emits its body, and if so where the definition goes and with what linkage.
    //
    // one taxonomy behind three readers that were each spelling their own subset of it: both loops of
    // TypeLowering::build_function_maps and StmtCodegen::gen_function_decl. They agreed on the easy
    // cases and drifted on the rest - the body emitter's last arm was a bare `if (!is_generic())
    // return;` that swallowed every shape nobody had thought about, so a declaration the loops had
    // *declared* and this one silently refused to define became a link error with nothing pointing at
    // it. Deriving all three from one enum is what makes the set total: a new kind that forgets an arm
    // does not compile.
    //
    // the split between t_extern_symbol and t_intrinsic looks redundant - neither has a body of ours -
    // but the two are declared at different moments and that difference is load-bearing. An extern is a
    // name, so it costs nothing to declare eagerly. Resolving an intrinsic is a signature match against
    // LLVM's whole intrinsic table, so it is declared only where it is referenced, which is what keeps a
    // program that touches no math from paying for every row of stdlib/std/math/intrinsics.eco.
    enum class FunctionEmission
    {
        // no symbol exists anywhere, so nothing may declare or define one - a `declare` nobody defines
        // fails at link time rather than at the mistake. Three shapes reach this: a `#[builtin:]`, which
        // is answered at each call site; an interface requirement, whose implementors hold the bodies
        // under their own symbols; and a generic template, which has no concrete signature to mangle.
        t_no_symbol,

        // declared inside `extern { }`: the symbol lives in another object file under its raw, unmangled
        // name. Declared eagerly, defined never.
        t_extern_symbol,

        // `#[intrinsic:]`: still a real llvm::Function, but LLVM supplies it. Declared lazily, on
        // reference only, for the cost reason above.
        t_intrinsic,

        // an ordinary function somebody wrote. Defined exactly once, with external linkage, in the unit
        // belonging to the module that declares it.
        t_module_local,

        // a definition the compiler generated rather than read: a field-wise constructor, a synthesized
        // class deinit or copy constructor, or a generic instantiation. Nobody wrote it, so nobody owns
        // it - every build that needs one regenerates it from the same inputs, which means two units can
        // legitimately both want the same symbol. That makes it exactly C++'s implicitly-instantiated
        // case, and it takes the same answer: `linkonce_odr`, defined in every unit that references it.
        //
        // the obligation that buys is real - two definitions under one ODR symbol must be *identical*,
        // or the linker keeps an arbitrary one. So nothing that lands here may depend on ambient
        // compiler state, only on the declaration and its substitution.
        t_odr_shared,
    };

    // the sole owner of the question. Defined out of line because it asks is_interface_requirement(),
    // which needs ComplexType complete.
    FunctionEmission function_emission_kind(const FunctionDeclNode *decl);

    // a reference to this declaration from some unit needs that unit to name the symbol. True for
    // everything that has one - the lazy half of build_function_maps, and the on-demand path in
    // ExprCodegen::find_llvm_function.
    bool emission_needs_declaration(FunctionEmission kind);

    // this compiler emits the body. Both readers of that are the same question asked from the two ends:
    // build_function_maps has to mint the symbol before a body can be attached to it, and
    // StmtCodegen::gen_function_decl is the one attaching it. They were two predicates over one switch
    // until a test had to pin them as equal - which is the shape of a second answer, not of two questions.
    bool emission_has_body(FunctionEmission kind);
};

#endif
