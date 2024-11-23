#ifndef RELEASENODE_H
#define RELEASENODE_H

#pragma once

#include "ASTNode.h"
#include "ExprNode.h"

namespace AST
{
    // one strong reference less on the class instance `target` names, tearing the block down if it was
    // the last.
    //
    // the class counterpart of the destructor call AST::OwnershipPass::emit_drop inserts for an owning
    // struct, and it appears in exactly the same places for exactly the same reasons: at the end of
    // every scope for each live class local, in reverse declaration order, and ahead of every `return`
    // for every enclosing scope at once. a moved-from local gets none - the reference travelled with
    // the value.
    //
    // a statement rather than an expression: it yields nothing, and nothing may consume it. that is
    // also why it is absent from NodeReference::is_expression_node(), which is what gen_scope keys its
    // value-stack bookkeeping on.
    //
    // deliberately *not* a call to a synthesized Echo function the way a struct's drop is. the
    // decrement, the zero test and the free are not expressible in Echo - there is no syntax for the
    // block header - so codegen owns the sequence, and shares one thunk per class between here and an
    // overwritten field. what *is* shared with the struct path is the part that matters: which types
    // owe cleanup, and when
    class ReleaseNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_release);

        // the place holding the reference to drop. a place rather than a value because the release has
        // to read the slot at the moment it runs: an `if` may have overwritten it since
        ExprNode *target = nullptr;

        ReleaseNode(ExprNode *target) : target(target)
        {
            assert(target != nullptr && "ReleaseNode requires a target");
        };

        ~ReleaseNode() {};

        const std::string node_description() override {
            return "release(" + target->node_description() + ")";
        }

        void accept(Visitor &visitor) override {
            visitor.visit_release(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};

#endif
