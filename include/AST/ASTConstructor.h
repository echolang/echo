#ifndef ASTCONSTRUCTOR_H
#define ASTCONSTRUCTOR_H

#pragma once

#include "Token.h"

#include <string>
#include <vector>

namespace AST
{
    class AssignNode;
    class Collector;
    class ExprNode;
    class FunctionDeclNode;
    class Module;
    class ScopeNode;
    class TypeDeclNode;
    class TypeNode;
    class TypeRegistry;
    class VarDeclNode;
    class ValueType;

    // **what every constructor body is made of, whoever writes it.** four producers build one -
    // Parser::parse_constructor for a written `constructor(...)`, the synthesized memberwise one
    // AST::finalize_type_construction builds, an enum's case
    // constructor, and AST::OwnershipPass::ensure_copy_constructor - and none of them is the one the
    // reader happens to be looking at. so the rules live here rather than at the first of them: while
    // they did not, the third producer rediscovered them wrongly - it gave a class no allocation at
    // all, and nothing anywhere said so
    //
    // the first two below open and close a body; the seating functions are the writes in between, and a
    // producer that seats a property by hand is a producer deciding those ownership questions on its own

    // mints a constructor's `$this` and gives it its storage. a struct's is the plain stack slot
    // StmtCodegen::ensure_var_slot zero-fills; a class's is a fresh heap block with both counts
    // already at 1, carried in the declaration's **initializer**
    //
    // that one initializer is the entire difference between constructing the two storage classes -
    // everything after it, the property writes and the implicit `return $this`, is shared code
    //
    // So the caller must place the returned declaration **ahead of every statement of the body**.
    //
    // The *slot* is hoisted to the function's entry block whatever its position (ensure_var_slot, and
    // ScopeNode::clone clones a scope's declarations before its statements). But an initializer is a
    // statement and runs where it was written, so a class's field writes would otherwise store through a
    // handle that is still null. StmtCodegen::gen_var_decl says the same thing from the other side
    //
    // a body-local of **value** type, never the borrow a method's `$this` is: OwnershipPass moves a
    // returned local only when `!source->type().is_pointer()`, and that move is what stops the value
    // being constructed from being destroyed on the way out of the constructor
    VarDeclNode &declare_constructor_this(Module &module, TypeNode &self_type, const TokenReference &at);

    // implicit `return $this` unless the body already leaves the function (scope_always_leaves_function:
    // a `break` still owes `$this`). mint a fresh read, never share a node with a field write
    void close_constructor_body(Module &module, FunctionDeclNode &decl, VarDeclNode &this_decl);

    // `$<local>-><name>`, as a place - one step of a synthesized path, and one read of the local per
    // use.
    //
    // never one node shared between two uses: a node that sits in the tree twice has two parents, and
    // every pass that rewrites a child in place - AST::PointerAdjuster on a deref, AST::OperatorRewriter
    // on a member-access base - would rewrite it once per parent. it is also what lets a clone answer
    // "already cloned" with the one clone, two parents collapsing onto it
    ExprNode *make_member_place(
        Module &module,
        VarDeclNode &local,
        const std::string &name,
        const TokenReference &at
    );

    // **one property of a constructor's `$this`, seated from a value** - the whole of what a
    // synthesized constructor body is, past the two above, and what a property default becomes once
    // it has been cloned into a constructor.
    //
    // two of the three decisions live here and none of them is the caller's. a *pointer* property is
    // bound and not written through, a plain assignment to one meaning "store into the pointee" and
    // doing that over a slot nothing has seated writing through uninitialized memory. the write is an
    // **initialization**, so it is the one write a `const` property ever gets and AST::TypeChecker has
    // to let it through.
    //
    // handover is deliberately not this function's: a field-wise constructor's parameter was given to
    // it to become part of the value, so seat_property_from_parameter sets `hands_over_value` after
    // this returns. a property default is a fresh expression and is copied like any other
    // initialization, which is why a hand-written constructor still has to spell its own transfers
    AssignNode &seat_property_from_value(
        Module &module,
        VarDeclNode &self,
        const VarDeclNode &property,
        ExprNode *value,
        const TokenReference &at
    );

    // the parameter path: seats from a read of `parameter` and marks the write a handover. an enum's
    // case constructor is built out of the same call
    AssignNode &seat_property_from_parameter(
        Module &module,
        VarDeclNode &self,
        const VarDeclNode &property,
        VarDeclNode *parameter,
        const TokenReference &at
    );

    // clone each property's `init_expr` into `ctor` after `$this`, ahead of the author's body. the
    // recipe stays on the property (a move would steal it from the next constructor). skip a copy
    // constructor and any field the body already assigns
    void prepend_property_defaults(
        Module &module,
        TypeDeclNode &type,
        FunctionDeclNode &ctor,
        TypeRegistry &registry,
        ScopeNode &declaration_scope
    );

    // which free constructor a type is owed. any written `constructor` deletes it; otherwise it is
    // memberwise over the fields a caller could have assigned. field defaults are parameter defaults.
    // a private property without an initializer is not a reason to skip registration - `init` may
    // still derive it - and finalize_type_construction refuses to build the body if it is still blank
    enum class SynthesizedConstructorKind
    {
        t_none,
        t_memberwise,
    };

    SynthesizedConstructorKind synthesized_constructor_kind(const TypeDeclNode &type);

    // instance properties that can be implicit constructor parameters: not static, not private.
    // derived fields are filtered by implicit_constructor_parameters, not here - derived-ness is
    // a fact about `init`, not about the property
    bool is_implicit_constructor_parameter(const VarDeclNode &property);

    std::vector<VarDeclNode *> implicit_constructor_parameters(const TypeDeclNode &type);

    // the constructor body's `$this` local, which is `body->children.front()` by construction. null
    // when the body has not been built. one owner so planting and definite assignment cannot disagree
    VarDeclNode *constructor_this(FunctionDeclNode &ctor);

    // insert a resolved call to `init` immediately before every `return` that leaves `ctor`,
    // including the implicit `return $this`. nested functions are a different frame and are skipped.
    // a `die` path does not return, so `init` does not run
    void plant_init_call(Module &module, FunctionDeclNode &ctor, FunctionDeclNode *init);

    // register the memberwise constructor in the function registry. asked from the declaration
    // pass so a `Foo(...)` in another file's body is not UnknownFunction. the body is not built
    // here: derived fields are unknown until every `init` in the module exists, so the arity may
    // still shrink. parse_funccall leaves construction calls unresolved until
    // finalize_module_construction has run
    void ensure_synthesized_constructor(
        Module &module,
        TypeDeclNode &type,
        Collector &collector,
        const ValueType &self_type
    );

    // **after every file's body pass.** `init` bodies exist, so derived fields are known: revive a
    // constructor the declaration pass skipped for an uninitialized private, drop parameters
    // `init` now derives, build the body once, then prepend, consume, plant, and the two
    // construction diagnostics that are not path-sensitive
    void finalize_type_construction(
        Module &module,
        TypeDeclNode &type,
        Collector &collector,
        ScopeNode &declaration_scope,
        const ValueType &self_type
    );

    // walk every type in the module from its file root and finalize_type_construction. asked once
    // from ModuleParser after pass 3 of every file, so constructor arity is stable before the
    // semantic pipeline settles `Foo(...)`
    void finalize_module_construction(Module &module, Collector &collector);

    // a private instance property with no initializer. the implicit ctor cannot take it as a
    // public argument and cannot leave it unassigned. null when every private field is initialized
    // or there are none
    const VarDeclNode *uninitialized_private_property(const TypeDeclNode &type);

    // **drops each property's `init_expr` once prepend_property_defaults has seated a clone.**
    // the recipe is not a live initializer: RecursiveVisitor::visit_type_decl would type-check it
    // again as a declaration, so a bad default reports twice at the same token. asked of
    // TypeDeclNode::defaults_cloned, not re-derived from whether a constructor exists - a bodyless
    // user constructor never cloned, and a copy-only type never cloned, so those leftovers stay for
    // TypeChecker to see
    void consume_property_defaults(TypeDeclNode &type);
};

#endif
