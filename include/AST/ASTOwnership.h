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
    class FunctionCallExprNode;
    class NodeReference;

    // where a value is arriving. two of the five take a place as an implicit move; at the other
    // three a place would be a copy:
    //
    //  - **t_return** is the one the whole feature rests on. a local is destroyed at the end of its
    //    scope and `return` *is* the end of its scope, so a returned local either moves or is
    //    destroyed out from under the caller. no `mv` is written because there is nothing else it
    //    could be
    //  - **t_initialization** is the synthesized field-wise constructor writing its parameters into
    //    the struct it is building (AssignNode::is_initialization). the user wrote none of it, so
    //    there is nowhere to put a `mv`, and the meaning is unambiguous: the parameter was handed
    //    over to be built into the struct, and the constructor is the only thing that ever writes
    //    that field. a *hand-written* constructor is not this - it says `$this->data = mv $data`,
    //    because there the transfer is a thing the author can and should be able to see
    enum class ValueDestination
    {
        t_declaration,
        t_assignment,
        t_initialization,
        t_argument,
        t_return,
    };

    // single ownership, as book/concept/ownership_and_moving.md specifies it: one owner per value,
    // destroyed exactly once when that owner's scope ends, `mv` to hand it elsewhere. three jobs in
    // one tree walk, because they answer each other:
    //
    //  - **copy or move.** at each place a value arrives - a declaration's initializer, an
    //    assignment, an initialization, a call argument, a `return` - a *place* source is a copy and
    //    a non-place source is already a move. for an owning type the copy is rejected (below) and
    //    `mv` marks the source moved-from. `return $local` is a move with no `mv`, and that is the
    //    rule the feature rests on: a constructor's `$this` is a body-local with an implicit
    //    `return $this`, so without it `$a = Buffer(...)` frees the buffer twice
    //  - **moved state.** a set of moved-from declarations carried down the walk and merged by union
    //    at a branch, so a move on one arm of an `if` leaves the variable unset after it. reading a
    //    moved local is a located error rather than a runtime surprise
    //  - **drops.** at every scope end and before every `return`, a destructor call per live local in
    //    reverse declaration order, skipping whatever was moved out
    //
    // **every drop is an ordinary FunctionCallExprNode in the tree**, receiver `&$local` - the same
    // choice AST::PointerAdjuster makes about derefs, and it buys the same three things: codegen
    // needs no scope-exit machinery, -ar shows the drops, and AST::TypeChecker validates what this
    // pass inserted (-a is the tree as *parsed*, before this pass runs at all)
    //
    // **a copy is a call to the type's copy constructor**, when it has one. the chapter's member-wise
    // deep copy is not what happens and never was - that recursion has no bottom, since every owning
    // type bottoms out at a raw `ptr<uint8>` the type system knows nothing about, so copying it
    // member-wise would leave two owners freeing one allocation. the type holding the pointer is the
    // one that knows, and a constructor taking a borrow of its own type is it saying so. recognised
    // rather than newly spelled (AST::is_copy_constructor), so `$b = $a` and `Foo($a)` are one
    // declaration; honoured whether or not the type owns anything, so which of the two a copy means
    // does not depend on whether a destructor happens to be declared. with no copy constructor, an
    // implicit copy of an *owning* type is still a located error naming `mv`, a borrow, and now the
    // third option
    //
    // **runs inside the monomorphizer's fixpoint**, beside the "re-derive a declaration's type from
    // its initializer" step. it has to: whether a `T $x` needs destroying is unknown until
    // substitution, and inserting a drop *creates* a generic call site - a `Box<int32>` local's drop
    // names the template's destructor, and the next round instantiates it through the ordinary path
    // so no instantiation logic lives here
    class OwnershipPass
    {
    public:
        OwnershipPass(Bundle &bundle);

        // resolves every function body (and file root) that is concrete and not yet processed
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

        // the storage this body has already *initialized* - a declaration and the member names below
        // it, as one key. an initialization owes the old value no teardown because there is no old
        // value, and this is what keeps that claim honest: a second write to the same owning field
        // would leak what the first one built, with nothing further down able to notice
        std::unordered_set<std::string> _initialized_storage;

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
        // now owns it
        //
        // nothing is ever inserted *ahead* of the statement being walked, and there is no longer a
        // list to insert it into. a drop that belongs to a statement is carried on it
        // (AssignNode::teardown_old); a scope's and a return's drops are appended by walk_scope. that
        // is not tidiness - inserting ahead of a statement is exactly what made an assignment tear its
        // target down before the right-hand side had read it
        NodeReference walk_statement(const NodeReference &child);

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

        // a resolved call to `callee` with `place` as its borrow receiver, positioned at `at`
        //
        // the receiver is addressed here, exactly as the parser addresses a method's: the parameter is
        // the borrow `Foo&`, and a value ranked against it would be no fit at all. unless the place
        // already *is* that address - the `$this` of a synthesized class deinit, declared `Foo&`,
        // which would otherwise be handed a ptr<ptr<Foo>>
        //
        // `decl` is set directly rather than resolved: there is no name to look up and no overload set
        // to search. for an instantiation it is the *template's* declaration, and the monomorphizer's
        // next round binds the owner's parameters from the receiver and rewires the call to the
        // instance - which is the whole reason this pass runs inside that fixpoint
        //
        // shared by the two things this pass inserts that call a member: a drop and a copy. they
        // differ only in which declaration they name and where the place comes from
        FunctionCallExprNode &emit_resolved_member_call(
            FunctionDeclNode *callee, const TokenReference &at, ExprNode *place);

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

        // --- synthesized declarations ------------------------------------------------------------

        // the skeleton every declaration this pass writes shares: a node named `name` at `site`, with
        // nothing left open for the monomorphizer to bind and an empty body attached. the caller fills
        // in what differs - the kind, the return type, the parameters, the statements
        //
        // `site` is the release or the copy that asked for the declaration, rather than the type's own
        // line: a synthesized declaration at line 0 gives every diagnostic raised inside its body
        // nowhere to point
        FunctionDeclNode &begin_synthesized_decl(const std::string &name, const TokenReference &site);

        // a single non-nullable borrow parameter, which is what both synthesized declarations take -
        // a deinit's `$this` and a copy constructor's `$other`. `Foo&` rather than `Foo`: a by-value
        // parameter of an owning type is an owner, and neither of them may own its argument
        VarDeclNode &add_borrow_parameter(
            FunctionDeclNode &decl, const std::string &name, const ValueType &borrowed, const TokenReference &site);

        // hands a finished declaration to the file root, and marks the round changed so the next one
        // walks its body
        //
        // through `_pending_declarations` rather than add_funcdecl directly: run_round is iterating
        // the very children this appends to. codegen emits a body only for a declaration that is one
        // of them, so a synthesizer that skips this step emits a `declare` nobody defines
        void publish_synthesized_decl(FunctionDeclNode &decl);

        // --- classes ---------------------------------------------------------------------------

        // the function a class's release calls when the count reaches zero: its own destructor, then
        // each owning property. synthesized on demand - the first time a release of this class is
        // inserted - and only when class_needs_deinit says the payload owns anything, so a plain data
        // class gets none and its release is a decrement and a free
        //
        // built out of emit_destructor_call and emit_property_drops, the same two pieces a struct's
        // scope-exit drop is built from. that is the point: what a class tears down at zero and what a
        // struct tears down at scope end are one decision, not two that have to be kept in step
        //
        // `site` is the release that asked for it, and it is what the synthesized declaration and its
        // `$this` are positioned at. a deinit is shared by every release of the class, so this is the
        // first one that needed it rather than the class's own declaration - which is still a real
        // line in the file that tears this class down, and a diagnostic raised inside the body has
        // somewhere to point other than line 0
        void ensure_class_deinit(const ValueType &class_type, const TokenReference &site);

        // --- copies ----------------------------------------------------------------------------

        // the copy constructor for a struct whose owning properties are all classes, transitively:
        // the body its author would have written, which is a field-wise assignment and nothing else
        //
        // no retain appears in what this builds. `$this->a = $other->a` is an ordinary assignment, so
        // the next round's walk reaches it through resolve_value_arrival and inserts the retain there
        // - the same arm a hand-written copy constructor's body goes through. a property that is a
        // struct with a copy of its own gets a resolved call to it instead, and one that needs a
        // synthesized copy asks for its own here, which is where the recursion lives
        //
        // synthesized on demand at the first copy that needs it, and per *concrete* type rather than
        // per template: whether the compiler can write the body at all depends on the property types,
        // and `Box<Handle>` can while `Box<Buffer>` cannot. AST::copy_is_synthesizable (ASTCopy.h) is
        // the rule, and it declines a type that already has a written one
        //
        // deliberately **not** registered in AST::FunctionRegistry, unlike the parser's field-wise
        // constructor. that registry is read only while parsing, and a call site is resolved as it is
        // parsed - so a declaration created inside this fixpoint arrives after every written call was
        // already resolved or already reported. it would make `Pair($p)` no more callable than it is
        // now, and would put a per-instantiation declaration into a name-keyed overload set the
        // parser owns. `$q = $p` is the spelling
        void ensure_copy_constructor(const ValueType &type, const TokenReference &site);

        // declarations synthesized this round - class deinits and copy constructors - appended to the
        // file root after the walk rather than during it, since resolve_function is iterating those
        // children. one list because it is one mechanism: whatever lands here is emitted by codegen
        // and walked by the next round like any other declaration
        std::vector<FunctionDeclNode *> _pending_declarations;
    };
};

#endif
