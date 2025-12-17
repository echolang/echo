#ifndef FOREACHNODE_H
#define FOREACHNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ExprNode.h"
#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "Token.h"

#include <optional>

namespace AST
{
    // `foreach ($a as $el) { ... }` and its keyed and borrowing spellings.
    //
    // **transient.** AST::ForeachLowering rewrites it inside the monomorphizer's fixpoint into the
    // iterator declaration and the `while` a hand-written loop would have been - so it is visible with
    // `-a` and gone by `-ar`, and needs no codegen, no ownership rule and no control-flow rule of its
    // own. PointerAdjuster and LLVMCompiler both throw if one reaches them, on AST::PointerValueNode's
    // contract: a marker a pass was supposed to erase is a compiler bug, and answering with something
    // plausible would hide it
    //
    // deliberately **not** in NodeReference::is_expression_node(): this is a statement, like n_scope,
    // n_while_statement and n_guard beside it. the CLAUDE.md trap reads as unconditional and is not
    class ForeachNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_foreach);

        // how `$el` is bound. **decided at the loop, not by the protocol** - the same collection is read
        // in one loop and written through in the next, and only the loop knows which. the iterator's
        // `current()` always hands back a borrow; what this chooses is what happens to it
        enum class Binding
        {
            // `$el` - a copy, elided to `const V&` when nothing in the body writes it
            t_value,

            // `&$el` - a borrow into the element, so a write reaches the collection
            t_borrow,

            // `const &$el` - the same borrow, read-only and said so
            t_const_borrow,
        };

        // the collection, or an iterator, as written
        ExprNode *source = nullptr;

        // `$k`, or null when the keyless form was written. always bound by value - a key is an index or
        // a hash key, and there is nothing to borrow into
        VarDeclNode *key = nullptr;

        // `$el`
        VarDeclNode *element = nullptr;

        ScopeNode *body = nullptr;

        Binding binding = Binding::t_value;

        // TokenReference has no copy *assignment* (only construction), so the one token that is known
        // before anything is parsed comes through the constructor and the rest are emplaced
        TokenReference token_foreach;

        // **no `as` token.** the two optionals below are here because a refusal points at them; the `as`
        // is grammar with nothing to say about a program, and a token nothing anchors to is a field
        // clone and every future reader still have to carry

        // the `&` or the `const`, when one was written - so a refusal about the binding mode lands on
        // the token that asked for it rather than on the whole statement
        std::optional<TokenReference> token_binding;

        // the `=>` of the keyed form, which is where "this iterator declares no key contract" belongs
        std::optional<TokenReference> token_arrow;

        // **no `lowered` flag.** every terminal path replaces this node's edge in its parent scope, so
        // the removal *is* the idempotency and one still in the tree is by construction still undecided.
        // that is the one place this departs from IndexExprNode and ArrayLiteralExprNode, which
        // legitimately survive their own decision

        ForeachNode(TokenReference token_foreach) : token_foreach(token_foreach) {}
        ~ForeachNode() {}

        const std::string node_description() override {
            std::string desc = "foreach (";
            desc += source != nullptr ? source->node_description() : "[none]";
            desc += " as ";

            if (key != nullptr) {
                desc += key->name_full() + " => ";
            }

            if (binding == Binding::t_const_borrow) {
                desc += "const &";
            }
            else if (binding == Binding::t_borrow) {
                desc += "&";
            }

            desc += element != nullptr ? element->name_full() : "[none]";
            desc += ")\n";
            desc += body != nullptr ? body->node_description() : "[no body]";

            return desc + "\n";
        }

        void accept(Visitor &visitor) override {
            visitor.visit_foreach(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};

#endif
