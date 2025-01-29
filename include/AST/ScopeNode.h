#ifndef SCOPENODE_H
#define SCOPENODE_H

#pragma once

#include "ASTNode.h"

#include <unordered_map>

namespace AST 
{
    class VarDeclNode;
    class FunctionDeclNode;
    class TypeDeclNode;
    class AttributeNode;

    class ScopeNode : public Node
    {
        std::unordered_map<std::string, VarDeclNode *> _declared_variables;

    public:

        // what a variable name resolves to, and whether reaching it left the frame it was asked from.
        //
        // two readers want different halves of that. a declaration asks so it can tell a *new* variable
        // from an assignment to an existing one, and a hit past a function boundary is not its business -
        // that is a fresh declaration shadowing an outer name. a *read* asks so it can tell an ordinary
        // outer-scope read from a capture, which Echo cannot lower yet
        struct VariableLookup
        {
            VarDeclNode *decl = nullptr;

            // how many function frames the lookup left to reach the declaration. the chain is
            // deliberately not cut at a boundary: it is the environment a closure captures from, so a
            // boundary is a marker rather than a wall
            //
            // a *count* and not a flag, because one and more-than-one are different situations: one is a
            // capture from the frame the closure is created in, which is the only kind that can be
            // evaluated at the creation site. more than one is transitive capture, which needs the
            // intervening closure to capture it too
            size_t boundaries_crossed = 0;

            // the declaration is at the file root rather than in any body. it is still one frame out -
            // file-scope statements are lowered as the implicit entry point's, so its storage is a local
            // of a function nobody wrote - and a diagnostic that says "an enclosing function" about it
            // names something the source does not contain
            bool declared_at_file_scope = false;

            inline bool crossed_function_boundary() const {
                return boundaries_crossed > 0;
            }

            // a usable answer *here*, which is a hit that did not have to leave the frame
            inline bool found_in_frame() const {
                return decl != nullptr && boundaries_crossed == 0;
            }
        };

        ScopeNode *parent_ptr = nullptr;

        // this scope holds a function's parameters, so everything above it belongs to another frame.
        // set on a function's, constructor's and destructor's argument scope
        bool is_function_boundary = false;

        NodeReferenceList children;

        ScopeNode() {};
        ~ScopeNode() {};

        ECO_AST_NODE_TYPE(n_scope);

        const std::string node_description() override;
        const std::string node_description_inner();

        void accept(Visitor &visitor) override {
            visitor.visitScope(*this);
        }

        Node *clone(CloneContext &cc) const override;

        inline ScopeNode &parent() const {
            assert(parent_ptr);
            return *parent_ptr;
        }

        inline bool is_root() const {
            return parent_ptr == nullptr;
        }

        inline bool is_leaf() const {
            return children.empty();
        }

        inline bool is_parent_of(const ScopeNode &node) const {
            for (const auto &child : node.children) {
                if (child.node() == this) {
                    return true;
                }
            }
            return false;
        }

        inline bool is_child_of(const ScopeNode &node) const {
            return parent_ptr == &node;
        }

        inline void add_child_scope(ScopeNode &child) {
            children.push_back(AST::make_ref(child));
            child.parent_ptr = this;
        }

        void add_vardecl(VarDeclNode &vardecl);

        // appends the declaration to the child list so codegen emits its body. it does *not*
        // register the name - functions are looked up through AST::FunctionRegistry, which keys
        // (namespace, name) to an overload set rather than to a single declaration
        void add_funcdecl(FunctionDeclNode &funcdecl);

        void add_typedecl(TypeDeclNode &structdecl);

        void add_attribute(AttributeNode &attribute);

        std::vector<AttributeNode *> collect_attributes();

        // resolves a variable name outward through the enclosing scopes, reporting whether it had to
        // leave this frame to find it. the one variable lookup - a plain "did you find it" would make
        // the two callers of this indistinguishable, and they mean different things by a hit from
        // outside the frame
        VariableLookup lookup_variable(const std::string &varname) const;

    private:

        // a list of attributes currently collected in the scope
        // its in the responsibilty of other parsers to detect if the attributes in the current scope apply 
        // to their context and to consume them from the scope
        std::vector<AttributeNode *> _attribute_stack;

    };
};

#endif