#ifndef STRINGINTERPOLATIONNODE_H
#define STRINGINTERPOLATIONNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ExprNode.h"
#include "AST/ASTValueType.h"

#include "Token.h"

#include <optional>
#include <string>
#include <vector>

namespace AST
{
    // a `"..."` literal with at least one `{$...}` hole in it, as written.
    //
    // **transient**: `AST::InterpolationLowering` replaces every one of these inside the
    // monomorphizer's fixpoint with the calls a hand written program would have spelled, and one
    // surviving the fixpoint is a bug rather than a shape codegen has to know. that is the same
    // bargain AST::ForeachNode makes, and it buys the same thing - `-p ast` shows the literal the
    // author wrote, `-p ast-resolved` shows what it became, and nothing downstream carries an arm
    // for interpolation.
    //
    // the node exists at all rather than the parser building the calls directly because *how* a
    // hole is rendered is one decision, in one place: the fold below can become a builder without
    // a single call site moving
    class StringInterpolationExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_string_interpolation);

        // one `{$...}`
        struct Hole
        {
            ExprNode *expr = nullptr;

            // the text after the hole's top level `:`, decoded, without the colon. nullopt when the
            // hole was written without one - which is a *different* call than an empty spec, since
            // `{$x:}` asks the spec taking overload for its default and `{$x}` does not ask it at all
            std::optional<std::string> spec;

            // where the hole's expression starts, for a diagnostic about the hole rather than about
            // the whole literal
            TokenReference token;
        };

        // the decoded text around the holes. **`chunks.size() == holes.size() + 1`, always** - an
        // empty chunk is how "two holes with nothing between them" is spelled, so the invariant
        // holds without a special case anywhere
        std::vector<std::string> chunks;
        std::vector<Hole> holes;

        // the `t_string_interp_begin` token: where the literal starts, quote included
        TokenReference token_string;

        // the `#[core: string]` type, stamped at construction for LiteralStringExprNode's reason -
        // `result_type()` is const and takes nothing, so it cannot go looking for one
        std::optional<ValueType> core_string_type;

        StringInterpolationExprNode(TokenReference token) :
            token_string(token)
        {};
        ~StringInterpolationExprNode() {};

        // **an interpolated literal is a `string`, exactly as a plain one is.** with no stdlib there
        // is no such type and no way to concatenate anything either, so this answers unknown and
        // AST::InterpolationLowering refuses the literal with a sentence naming the standard library
        ValueType result_type() const override {
            if (core_string_type.has_value()) {
                return core_string_type.value();
            }

            return ValueType();
        }

        void accept(Visitor &visitor) override {
            visitor.visit_string_interpolation(*this);
        }

        Node *clone(CloneContext &cc) const override;

        const std::string node_description() override {
            std::string desc = "interpolation(";

            for (size_t i = 0; i < chunks.size(); i++) {
                desc += "\"" + chunks[i] + "\"";

                if (i < holes.size()) {
                    desc += " . ";
                    desc += holes[i].expr != nullptr ? holes[i].expr->node_description() : "[none]";

                    if (holes[i].spec.has_value()) {
                        desc += ":" + holes[i].spec.value();
                    }

                    desc += " . ";
                }
            }

            return desc + ")";
        }
    };
};

#endif
