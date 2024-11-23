#ifndef ASTOWNERSHIP_H
#define ASTOWNERSHIP_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTValueType.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace AST
{
    class Bundle;
    class Collector;
    class Module;
    class File;
    class ScopeNode;
    class ExprNode;
    class VarDeclNode;
    class FunctionDeclNode;
    class NodeReference;

    // where a value is arriving. two of the five take a place as an implicit move; at the other
    // three a place would be a copy:
    //
    //  - **t_return** is the one the whole feature rests on. a local is destroyed at the end of its
    //    scope and `return` *is* the end of its scope, so a returned local either moves or is
    //    destroyed out from under the caller. no `mv` is written because there is nothing else it
    //    could be.
    //  - **t_initialization** is the synthesized field-wise constructor writing its parameters into
    //    the struct it is building (AssignNode::is_initialization). the user wrote none of it, so
    //    there is nowhere to put a `mv`, and the meaning is unambiguous: the parameter was handed
    //    over to be built into the struct, and the constructor is the only thing that ever writes
    //    that field. a *hand-written* constructor is not this - it says `$this->data = mv $data`,
    //    because there the transfer is a thing the author can and should be able to see
    enum class ValueDestination {
        t_declaration,
        t_assignment,
        t_initialization,
        t_argument,
        t_return,
    };

    // single ownership, as book/concept/ownership_and_moving.md specifies it: one owner per value,
    // destroyed exactly once when that owner's scope ends, and `mv` to hand the ownership somewhere
    // else. three jobs, one tree walk, because they answer each other:
    //
    //  - **copy or move.** at each of the places a value arrives - a declaration's initializer, an
    //    assignment, an initialization, a call argument, a `return` - a *place* source would be a copy and a
    //    non-place source is already a move. for a type that owns something the copy is rejected
    //    (see below) and `mv` marks the source moved-from. `return $local` is a move with no `mv`,
    //    which is the rule the whole feature rests on: a constructor's `$this` is a body-local with
    //    an implicit `return $this`, so without it `$a = Buffer(...)` frees the buffer twice.
    //  - **moved state.** a set of moved-from declarations, carried down the walk and merged by
    //    union at a branch, so a move on one arm of an `if` leaves the variable unset after it.
    //    reading a moved local is a located error rather than something found at runtime.
    //  - **drops.** at the end of every scope and before every `return`, a destructor call per live
    //    local in reverse declaration order, skipping whatever was moved out.
    //
    // **every drop is an ordinary FunctionCallExprNode in the tree**, receiver `&$local`, the same
    // node a hand-written call would be. that is the same choice AST::PointerAdjuster makes about
    // derefs and it buys the same three things: codegen needs no scope-exit machinery at all,
    // --print-resolved-ast shows the drops, and AST::TypeChecker validates the calls this pass
    // inserted. (--print-ast is the tree as *parsed*, which is before this pass has run at all.)
    //
    // **an implicit copy of an owning type is rejected**, which is where this deviates from the
    // chapter. the chapter specifies a member-wise deep copy, but that recursion has no bottom: the
    // leaf of every owning type is a raw `ptr<uint8>` field, which owns nothing as far as the type
    // system can tell, so member-wise copying it is a shallow pointer copy and both destructors free
    // it. the chapter already lists a user-written copy constructor and nested owners as
    // unspecified; until one of them exists, `$b = $a` on an owning type is a located error naming
    // `mv` rather than a double free.
    //
    // **runs inside the monomorphizer's fixpoint**, one more step beside the "re-derive a
    // declaration's type from its initializer" one. it has to: whether a `T $x` needs destroying is
    // not known until substitution, and inserting a drop *creates* a generic call site - the drop
    // for a `Box<int32>` local calls the template's destructor, and the next round unifies the
    // receiver and instantiates it through the ordinary path. so no instantiation logic lives here.
    class OwnershipPass
    {
    public:
        OwnershipPass(Bundle &bundle);

        // resolves every function body (and file root) that is concrete and not yet processed.
        // answers whether anything changed, so the fixpoint can report progress. idempotent: a body
        // is processed exactly once, and a body still mentioning a type parameter is left for a
        // later round
        bool run_round();

    private:
        // the destructible locals of one lexical scope, in declaration order. a `return` unwinds
        // every frame from the innermost out, which is why this is a stack and not a single list
        struct Frame
        {
            ScopeNode *scope = nullptr;
            std::vector<VarDeclNode *> locals;
        };

        Bundle &_bundle;
        Collector &_collector;

        Module *_current_module = nullptr;
        File *_current_file = nullptr;
        FunctionDeclNode *_current_function = nullptr;

        std::vector<Frame> _frames;

        // declarations whose value has been moved out. a moved local is neither readable nor
        // dropped - "its destructor travelled with the value"
        std::unordered_set<const VarDeclNode *> _moved;

        // locals moved inside a branch that did not certainly run. read a second time, so the
        // diagnostic can say "may have been moved" rather than claiming it definitely was
        std::unordered_set<const VarDeclNode *> _maybe_moved;

        // bodies already resolved, so the fixpoint can call this every round
        std::unordered_set<const FunctionDeclNode *> _processed_functions;
        std::unordered_set<const ScopeNode *> _processed_roots;

        bool _changed = false;

        CodeRef code_ref_for(const TokenReference &token);

        // a token this pass synthesizes, positioned at the token that asked for it. everything this
        // pass mints is a position and a name rather than an identity - no declaration site is keyed
        // on one - but a diagnostic still has to land somewhere the reader can find
        TokenReference virtual_token(const std::string &value, Token::Type type, const TokenReference &at);

        // --- the walk -------------------------------------------------------------------------

        void resolve_function(FunctionDeclNode &decl);
        void resolve_root(ScopeNode &root);

        // are every declaration in this body's types settled? a body is walked exactly once, so a
        // question asked before the monomorphizer has substituted is answered wrongly forever
        bool body_is_concrete(ScopeNode &scope) const;

        // walks the scope's statements, then appends the drops its own frame owes. a function body's
        // frame is pushed by resolve_function, which seeds it with the owning parameters; every other
        // scope opens its own here
        void walk_scope(ScopeNode &scope);

        // answers the statement the scope should keep in place of `child`. that is `child` itself for
        // everything except a discarded owning temporary, which is replaced by the declaration that
        // now owns it. `before` is the statement list walk_scope has built so far, so a statement this
        // pushes there lands immediately ahead of the one being walked
        NodeReference walk_statement(const NodeReference &child, NodeReferenceList &before);

        // binds a discarded owning value to a synthesized local of the enclosing frame, so the scope
        // destroys it. the frame's ordinary reverse-order drop then covers it with no special case
        VarDeclNode &bind_discarded_temporary(ExprNode *expr);
        void walk_expression(ExprNode *expr);

        // a local moved out of on one branch of an `if` but not the other. the chapter says the
        // variable is unset afterwards, which settles reading it - but not who destroys the value on
        // the branch that kept it. see the definition
        void report_conditional_move(const VarDeclNode *decl);

        // --- copy or move ---------------------------------------------------------------------

        // resolves one value-arrival site. `wanted` is the destination type and `param` the
        // parameter it is arriving at, or null when the destination is not one. answers the
        // expression to use in place of `expr` - the operand with the `mv` marker erased, or `expr`
        // unchanged
        ExprNode *resolve_value_arrival(
            ExprNode *expr, const ValueType &wanted, const VarDeclNode *param, ValueDestination destination);

        // --- drops ----------------------------------------------------------------------------

        // appends the drop statements for `frame`'s live locals, innermost value first: reverse
        // declaration order, as the chapter specifies
        void collect_frame_drops(const Frame &frame, std::vector<NodeReference> &out);

        // destroying a value of `type` at `root`->`path`: its own destructor if it has one, then
        // each property that needs destroying, in reverse declaration order. no implicit destructor
        // is synthesized for the member-wise part - it is inlined at the drop site, because whether
        // a property needs destroying is not answerable where a synthesized declaration would have
        // to be built (a generic property type is still open in the parser)
        //
        // `path` is the member path from `root` down to the value being destroyed, and it is one
        // vector pushed and popped in step with the recursion rather than a copy per property: a
        // deep struct graph would otherwise reallocate and re-copy the whole path at every level
        void emit_drop(
            VarDeclNode *root,
            std::vector<std::string> &path,
            const ValueType &type,
            std::vector<NodeReference> &out);

        // a fresh `$root->a->b` place for one drop. rebuilt per drop rather than shared, so no node
        // sits in the tree twice
        ExprNode *make_place(VarDeclNode *root, const std::vector<std::string> &path);

        // the destructor call for one value, when its type declares one. the receiver is the address of
        // the place - except when the place already *is* that address, which is the deinit's `$this`
        void emit_destructor_call(
            VarDeclNode *root,
            const std::vector<std::string> &path,
            const ComplexType *ct,
            std::vector<NodeReference> &out);

        // each property of `ct` that needs destroying, in reverse declaration order
        void emit_property_drops(
            VarDeclNode *root,
            std::vector<std::string> &path,
            const ComplexType *ct,
            std::vector<NodeReference> &out);

        // --- classes ---------------------------------------------------------------------------

        // the function a class's release calls when the count reaches zero: its own destructor, then
        // each owning property. synthesized on demand - the first time a release of this class is
        // inserted - and only when class_needs_deinit says the payload owns anything, so a plain data
        // class gets none and its release is a decrement and a free.
        //
        // built out of emit_destructor_call and emit_property_drops, the same two pieces a struct's
        // scope-exit drop is built from. that is the point: what a class tears down at zero and what a
        // struct tears down at scope end are one decision, not two that have to be kept in step.
        //
        // `site` is the release that asked for it, and it is what the synthesized declaration and its
        // `$this` are positioned at. a deinit is shared by every release of the class, so this is the
        // first one that needed it rather than the class's own declaration - which is still a real
        // line in the file that tears this class down, and a diagnostic raised inside the body has
        // somewhere to point other than line 0
        void ensure_class_deinit(const ValueType &class_type, const TokenReference &site);

        // deinits built this round, appended to the file root after the walk rather than during it -
        // resolve_function is iterating those children
        std::vector<FunctionDeclNode *> _pending_deinits;
    };
};

#endif
