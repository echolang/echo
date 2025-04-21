#ifndef LOOPCONTROLNODE_H
#define LOOPCONTROLNODE_H

#pragma once

#include "ASTNode.h"
#include "Token.h"

namespace AST
{
    enum class LoopControlKind
    {
        t_break,
        t_continue,
    };

    // `break;` and `continue;`.
    //
    // **one node with a kind, not two nodes.** the difference between them is exactly one bit and it is
    // read in exactly one place - which basic block CodegenContext::loop_targets hands back. the fields,
    // the clone, the parser shape, the unwind list, the control-flow arm and the visitor entry are
    // byte-identical, so a second class would be a second thing to keep in step for no question it could
    // answer differently. (`weak` is a ValueType *kind* rather than a flag for the opposite reason: there,
    // every reader has to grow an arm, and a flag would let them silently not.)
    class LoopControlNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_loop_control);

        LoopControlKind kind = LoopControlKind::t_break;

        // the `break` / `continue` keyword, so a diagnostic about the statement lands on it
        TokenReference token;

        // the drops this exit owes: every frame from the innermost out, down to **and including** the loop
        // body's. shorter than a return's by construction - the frames outside the loop are still live on
        // the other side of the branch, which is the whole difference between the two exits. filled by
        // AST::OwnershipPass, whose _loop_frames is what bounds the walk
        NodeReferenceList unwind;

        // **deliberately no pointer to the loop this leaves.** the target is already answered twice further
        // down - by AST::OwnershipPass's frame stack and by CodegenContext::loop_targets - and a third
        // answer stored here is the one that goes stale the moment the monomorphizer clones a loop body
        //
        // and deliberately not in NodeReference::is_expression_node(): this is a statement. putting it
        // there would make StmtCodegen::gen_scope take the "discard the statement's value" path and pop a
        // value that belongs to somebody else

        LoopControlNode() = default;
        LoopControlNode(LoopControlKind kind, TokenReference token) : kind(kind), token(token) {}
        ~LoopControlNode() {}

        const char *keyword() const {
            return kind == LoopControlKind::t_break ? "break" : "continue";
        }

        const std::string node_description() override {
            std::string buffer = keyword();

            // shown, because `-ar` is where a missing or duplicated drop is diagnosed
            for (auto &drop : unwind) {
                if (drop.has()) {
                    buffer += "\n  unwind " + drop.node()->node_description();
                }
            }

            return buffer;
        }

        void accept(Visitor &visitor) override {
            visitor.visit_loop_control(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};

#endif
