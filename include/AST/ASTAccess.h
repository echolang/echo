#ifndef ASTACCESS_H
#define ASTACCESS_H

#pragma once

#include "AST/ASTValueType.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AST
{
    class ExprNode;
    class VarDeclNode;
    class FunctionDeclNode;

    // **an address is not an access.**
    //
    // A `T&` or a `ptr<T>` is an address. It can be copied, stored, returned and compared, it may
    // alias anything, and on its own it promises the optimiser nothing.
    //
    // An *access* is the other half: a non-escaping capability over a region of storage, with a
    // beginning, an end, and an effect. This enum is the effect, and it is what AST::AccessPass weighs
    // two live accesses against each other with.
    //
    // Deliberately on VarDeclNode and not on ValueType, for the reason spelled out beside
    // `takes_ownership`. ValueType is the interning identity for TypeRegistry and for the
    // monomorphizer's instance cache, so a flag there would fork every borrowing type in two.
    //
    // The consequence is `mv`'s consequence: `f(inout T& $x)` and `f(T& $x)` mangle identically and
    // collide as a DuplicateFunctionSignature. That is correct for the same reason - an effect is a
    // contract about the argument, not a distinction a call can be resolved on
    enum class AccessEffect
    {
        // nothing said. a plain `T&` parameter, an address with C aliasing, exactly as every one of
        // them is today. **this is what keeps the rule out of existing programs**: the conflict check
        // needs *both* sides to have said something, so `swap(int32& $a, int32& $b)` is unchanged
        // until its author writes an effect on one
        t_none,

        // initialized on entry, initialized on exit, and **may overlap other reads** - the one
        // non-exclusive effect, and therefore the only one that is ever *inferred*: any `const T&`
        // is a read whether or not the word was written, because a read alone can never refuse
        // anything. exclusivity is always said out loud, by a mutating method's receiver or by a
        // keyword
        t_read,

        // initialized on entry, initialized on exit, exclusive. what an ordinary method's receiver is
        t_inout,

        // *un*initialized on entry, initialized on exit, exclusive. what a constructor's receiver is,
        // and what an append slot is. the reason it is worth a fourth case rather than a flavour of
        // inout: there is no old value to load and no destructor to run, which is a correctness
        // statement (the storage is not yet an owner) before it is an optimization
        t_out,

        // initialized on entry, consumed on exit, exclusive. `mv` is its spelling and a destructor's
        // receiver is its other producer - the only one of the four that already had a keyword
        t_take,
    };

    // **the one answer to "what access does this parameter take".**
    //
    // it folds three spellings the author has - the effect keyword, `mv`, and a receiver's own
    // declared const-ness - into one enum, so nothing downstream re-derives it from a token or from
    // args[0]'s type. that is the same split AST::receiver_is_const takes: a reader, not a store.
    //
    // the two-argument form is the one to ask when the position matters, because a *receiver* takes
    // its effect from what kind of member the function is rather than from anything written on it,
    // and only the declaration knows which one it is
    AccessEffect access_effect_of(const VarDeclNode &decl);
    AccessEffect access_effect_of(const FunctionDeclNode &decl, size_t index);

    // **the effect a caller has to write**, which is a different question from the one above and the
    // reason the two are not one function: a `const T&` *takes* a read and says so in its type, so a
    // signature that rendered the inferred answer would print `read const T&` - one fact twice, and
    // a word the author neither wrote nor needs to.
    //
    // read only by AST::FunctionDeclNode::signature_description. everything that reasons about
    // storage wants access_effect_of instead
    AccessEffect declared_access_effect(const VarDeclNode &decl);

    // exclusive means: for as long as this access is live, no other access may reach the region it
    // covers - not even a read. three of the four are
    inline bool access_is_exclusive(AccessEffect effect)
    {
        return effect == AccessEffect::t_inout
            || effect == AccessEffect::t_out
            || effect == AccessEffect::t_take;
    }

    // **do two accesses to one region conflict?**
    //
    // exclusivity is not symmetric with silence: `t_none` conflicts with nothing, because a parameter
    // that declared no effect made no promise anyone can be held to. so a conflict needs both sides
    // to have said something and at least one of them to have said something exclusive.
    //
    // that is what scopes the rule to the code it was written for. a method receiver is `t_inout` by
    // being a method, and a `const` one is `t_read` by being const, so `$a->extend($a)` conflicts
    // with nothing written anywhere - while `swap(&$x, &$y)`, whose parameters say nothing, is
    // untouched until its author says something
    inline bool access_effects_conflict(AccessEffect a, AccessEffect b)
    {
        if (a == AccessEffect::t_none || b == AccessEffect::t_none) {
            return false;
        }

        return access_is_exclusive(a) || access_is_exclusive(b);
    }

    // how the effect is written, for a diagnostic. `t_take` renders as `mv` because that is the
    // keyword an author would have to type, and `t_none` renders empty so a signature with no effect
    // has no gap in it
    const char *access_effect_spelling(AccessEffect effect);

    // ------------------------------------------------------------------------------------------
    // regions

    // one step of the path from a declaration to the storage an expression names. deliberately three
    // kinds and not one per node type: what the overlap rule needs to know is only whether two steps
    // are provably the *same* step, provably a *different* one, or neither
    enum class ProjectionKind
    {
        // `->name`. keyed on the name, which is enough because two paths are only ever compared once
        // they already share a root and every step before this one
        t_field,

        // `[i]` where AST::const_fold answered. the one step that can be told apart by a number
        t_element,

        // `[i]` where nothing folded. **never disjoint from anything**, including itself: two reads
        // of `$i` are the same index and two of `$i++` are not, and the path does not know which
        t_dynamic,

        // **somewhere in what a pointer read from here reaches** - `$a->storage->data:$[3]`.
        //
        // it exists so that a region *includes the storage its own pointers own*, which is what makes
        // `$src->storage->data:$[0] = 999` a write to `$src` rather than a write to nothing the
        // compiler can name. without it the path simply ends at the bracket and a `read` parameter
        // could be written through in silence.
        //
        // it is deliberately weaker than every other step: two of them are never provably the same
        // slot, and **a path containing one is never disjoint from a path under a different root** -
        // two structs holding one raw pointer are two roots naming one region, and nothing here can
        // see that they are
        t_indirect,
    };

    struct Projection
    {
        ProjectionKind kind = ProjectionKind::t_dynamic;

        // t_field
        std::string field;

        // t_element
        uint64_t index = 0;
    };

    // **the storage an expression names, said as a declaration plus a path to walk from it.**
    //
    // a null root means unknown provenance - the expression reached storage through something the
    // compiler is not tracking, most often a raw pointer - and *unknown is not disjoint*. every
    // answer this feeds is conservative in that direction
    struct AccessPath
    {
        VarDeclNode *root = nullptr;
        std::vector<Projection> projections;

        bool is_known() const {
            return root != nullptr;
        }
    };

    // three answers, and `t_unknown` is load-bearing rather than a tail: a rule that refuses on
    // "not disjoint" and one that optimises on "not overlapping" both read this, and collapsing the
    // middle answer into either one of them is wrong for the other
    enum class Overlap
    {
        t_disjoint,
        t_overlap,
        t_unknown,
    };

    // builds the path an expression names. transparent through everything that only re-addresses
    // existing storage - `&E`, `E:$`, an implicit cast - which is the same taxonomy
    // AST::place_root_of walks, one projection richer
    AccessPath access_path_of(ExprNode *expr);

    // the same walk, stopped at the declaration - for a caller that only asks *which storage*, never
    // which part of it. null where access_path_of would answer an unknown path.
    //
    // it shares that walk rather than repeating it because the two must agree on which expressions
    // name storage at all: a root the projection walk would refuse is a root no rule may act on
    VarDeclNode *access_root_of(ExprNode *expr);

    // **do these two paths reach storage that overlaps?**
    //
    // the whole rule, and the one place it is spelled. two declarations are distinct storage *unless*
    // either of them merely holds an address it was handed - a borrow, a `ptr<T>`, a class handle,
    // an erased interface - because two of those can name one object and nothing here can see that
    // they do. that exception is what keeps this honest at a *body* boundary: inside `extend`,
    // `$this` and `$other` are two roots and the answer is `t_unknown`, which is exactly right and
    // is the reason a call-site check cannot on its own license a `noalias`
    Overlap path_overlap(const AccessPath &a, const AccessPath &b);

    // does this declaration *own* the storage its name reaches, or merely address it? the exception
    // above, on its own, because Phase 5's lowering has to ask the same question about a parameter
    bool root_owns_its_storage(const VarDeclNode &decl);

    // ------------------------------------------------------------------------------------------
    // the safe/raw boundary

    // **is this expression's storage reached through a raw pointer?**
    //
    // the AST half of `Compiler::LLVM::LValue::provenance`, and the two must answer alike: that one
    // decides whether an access carries a type tag, this one decides whether forming a borrow of it
    // is a promotion. a `ptr<T>` deref or index is raw; a `T&` is not, because a borrow is already
    // the trusted form and the promise was made where it was minted
    bool place_is_raw_derived(ExprNode *expr);

    // **does forming this borrow promote raw storage into a trusted typed one?**
    //
    // this is the semantic boundary, and it is deliberately *not* pointer casting. `ptr<uint32>(&$f)`
    // only computes another raw address: reads and writes through a `ptr<T>` are untagged, so the
    // optimizer stays conservative around every one of them and nothing has been promised.
    //
    // What licenses a promise is the step that turns a raw address into a `T&`. From there the type is
    // the contract, every later access carries `AST::access_family_of(T)`, and it keeps carrying it
    // through however many function boundaries the borrow travels. That is the assertion no compiler
    // can check and an author can, so that is where the word goes.
    //
    // `to` is the borrow being formed, and `operand` is the **place** it is taken of - never the
    // operand's *type*: `&$p` on a `ptr<T>` local addresses an ordinary slot, and reading the type
    // instead would make every `ptr<ptr<T>>` in the language a promotion
    bool borrow_promotes_raw_storage(const ValueType &to, ExprNode *operand);

    // the explicit narrowing `T&($p:$)` - the one promotion that converts a *value* rather than
    // taking the address of a place, so it has a question and a call site of its own
    bool narrowing_promotes_raw_storage(const ValueType &from, const ValueType &to);

    // ptr<T> <-> extern function<R(P...)>. the same shape of promise as the narrowing above:
    // a raw word becomes a trusted typed callable, and the reverse extracts that word. both
    // directions: a loader stores a resolved address as ptr<uint8> and reads it back typed
    bool function_pointer_promotes_raw_storage(const ValueType &from, const ValueType &to);
};

#endif
