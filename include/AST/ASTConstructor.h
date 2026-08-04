#ifndef ASTCONSTRUCTOR_H
#define ASTCONSTRUCTOR_H

#pragma once

#include "Token.h"

namespace AST
{
    class FunctionDeclNode;
    class Module;
    class TypeNode;
    class VarDeclNode;

    // **the two things every constructor body is made of, whoever writes it.** three producers build
    // one - Parser::parse_constructor for a written `constructor(...)`, the field-wise one the type
    // declaration parser synthesizes, and AST::OwnershipPass::ensure_copy_constructor - and none of
    // them is the one the reader happens to be looking at. so the rules live here rather than at the
    // first of them: while they did not, the third producer rediscovered them wrongly - it gave a class
    // no allocation at all, and nothing anywhere said so

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

    // appends the implicit `return $this` that ends a constructor - unless control already leaves the
    // body on every path
    //
    // **AST::scope_always_leaves_function, not scope_always_exits.** a `break` leaves a scope without
    // leaving the constructor, so a body that only breaks still owes `$this`, and without one
    // gen_function_decl synthesizes `ret undef`. the parser refuses a `break` with no enclosing loop and
    // builds no node for it, so the two predicates cannot disagree here today - the boundary is named
    // while it is still free to name
    //
    // And not "does a child return" either. A body ending in `die`, or in an `if` whose arms all leave,
    // is already done, and a return appended behind it is written where nothing reaches.
    //
    // It never reaches the binary - StmtCodegen::gen_scope stops at the first terminated block - but the
    // ownership pass sees a ReturnNode and hangs a full unwind drop set on it. Dead drops that are still
    // type checked, and that still mint a generic call site for a generic local.
    //
    // Mints its own read, never sharing one node with a field write. A node that sits in the tree twice
    // has two parents, so every pass that rewrites a child in place rewrites it once per parent - and a
    // clone that answers "already cloned" collapses both parents onto the one clone
    //
    // the **explicit** `return;` written inside a constructor is not this one. Parser::parse_return
    // rewrites that where it is read, off Context::ctor_this_ptr, long before the body scope this takes
    // exists - a different moment, and the only thing the two share is the shape of the node
    void close_constructor_body(Module &module, FunctionDeclNode &decl, VarDeclNode &this_decl);
};

#endif
