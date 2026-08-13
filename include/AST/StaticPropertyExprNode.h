#ifndef STATICPROPERTYEXPRNODE_H
#define STATICPROPERTYEXPRNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ExprNode.h"
#include "AST/ASTValueType.h"

#include "Token.h"

#include <optional>
#include <string>

namespace AST
{
    class VarDeclNode;
    class FunctionDeclNode;

    // `Session::$count` - a read or a write of storage the *type* owns.
    //
    // **a place**, and the only one whose storage is neither a frame slot nor reached through a value:
    // there is one global per (type, property), so `AST::storage_of` answers `t_place` and
    // LValueCodegen hands back the global's address. everything a place already gets - `&`, an
    // assignment, a compound one, a borrow argument, a `foreach` source - follows with no arm anywhere.
    //
    // **it carries the owner as a ValueType rather than only the declaration**, and that is not
    // redundancy. one `VarDeclNode` on the template backs both `Box<int32>::$count` and
    // `Box<float>::$count`, which are two disjoint globals - so the declaration alone cannot say which
    // storage is meant, and answering it with the declaration would make two unrelated statics one.
    // the owner is also what `result_type()` substitutes the declared type through
    class StaticPropertyExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_static_property);

        // the `$name` token, which is where a diagnostic about this access points
        TokenReference token_name;

        // the type that owns the storage, already instantiated: `Box<int32>` and never `Box<T>`. it is
        // substituted on a clone like any other type a node carries, which is what makes a static named
        // inside a generic body mean the instance's storage rather than the template's
        ValueType owner;

        // the declaration, which lives on the template - see ComplexType::static_properties. null only
        // while a parse failed, and every reader guards the pointer for that reason
        VarDeclNode *decl = nullptr;

        // the property's position in its owner's static list. half the identity of the global's symbol,
        // paired with the owner's mangled token - so a rename of the property is a different symbol and
        // a reorder is not, which is the same bargain a struct field's index makes
        size_t index = 0;

        // the function that seats this static's value, synthesized by AST::OwnershipPass. carried on
        // the *access* rather than on the declaration because a declaration is shared and an access is
        // not: that is what will let a static on a generic owner have one init per instantiation with
        // nothing here to change, each access already knowing which owner it named
        //
        // null until the ownership pass has reached it, and null for a static with no initializer -
        // which is a legitimate answer, not a hole: the storage is zero-initialized either way
        FunctionDeclNode *init = nullptr;

        // the function that ends this static's value, synthesized beside the one above and only
        // for a type that owes a teardown at all. null is the common answer - an `int32` static
        // owes nothing - and it is what decides whether the init pushes a teardown node
        FunctionDeclNode *deinit = nullptr;

        StaticPropertyExprNode(const TokenReference &token_name, const ValueType &owner, VarDeclNode *decl, size_t index)
            : token_name(token_name), owner(owner), decl(decl), index(index)
        {}

        ~StaticPropertyExprNode() {}

        // the declared type, read through the owner. defined out of line, VarDeclNode being incomplete
        // here for the same reason ComplexType's members are incomplete in ASTValueType.h
        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_static_property(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
