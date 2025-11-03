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
    class MemberAccessNode;
    class NodeReference;

    // **the operand edge a pending temporary request means.** the owner holds its operand in one of two
    // shapes - a plain field for an `&` or a `?->`, a NodeReference for a member access's base so the
    // tag travels with the pointer - and that is the only thing the three callers differ in. spelled
    // once here so reading the operand and reseating it cannot disagree about which edge they mean
    struct PendingEdge
    {
        ExprNode **slot = nullptr;
        NodeReference *base = nullptr;

        ExprNode *get() const;
        void set(ExprNode *place) const;
    };

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
        // **what an enclosing position does with the storage requests raised beneath it**, opened
        // where the decision is made and closed over the expression the decision is about.
        //
        // there are three answers and only two of them are a scope, which is the point:
        //
        //  - **bind.** the position *reads a value*, so it outlives every request below it. closing
        //    mints the slots, reseats the operands and hangs the drops on the result
        //  - **refuse.** no lifetime a temporary can have is long enough, so binding one would be
        //    silently wrong rather than merely tight. closing reports and leaves the tree exactly as
        //    it was written, which is what lets the reader see the program they wrote
        //  - **forward** - *no scope at all.* the position keeps an **address** rather than reading a
        //    value, so it does not outlive the storage and the request travels one step further out.
        //    every place edge is one, and so is a call: when the value a call hands back is made of a
        //    temporary it borrowed, the call is not where the lifetime ends
        //
        // spelling "forward" as the absence of a scope is deliberate. it is the overwhelmingly common
        // case - every `&`, `->`, `[…]`, `:$` and `?->` in the program - so a walker arm says nothing
        // and gets the right answer, and a position that *does* decide has to say which decision it
        // is making. the two constructors are the two decisions: neither can be written without its
        // reason, a refusal without wording or a bind without an expression to hang the drops on
        //
        // a **frame** lifetime is deliberately not one of these. binding a value to the enclosing
        // frame is a rewrite of the *statement* rather than of an expression edge - the statement
        // becomes the declaration - so bind_discarded_temporary owns it, sharing make_temporary and
        // nothing else. one mint, two shapes
        class MaterializationScope
        {
        public:
            // the binding form. `pass` outlives it by construction: every scope is a local of a
            // method on the pass
            explicit MaterializationScope(OwnershipPass &pass);

            // the refusing form. `action` and `outcome` are the two halves the refusing positions
            // differ in - an address dangles, a write is simply lost - and they are constructor
            // arguments rather than a later call so a refusal cannot exist without saying why
            MaterializationScope(OwnershipPass &pass, const char *action, const char *outcome);

            ~MaterializationScope();

            MaterializationScope(const MaterializationScope &) = delete;
            MaterializationScope &operator=(const MaterializationScope &) = delete;

            // applies the decision and answers the expression the caller should use in place of
            // `value` - the same node when nothing was requested, which is every edge in almost every
            // program. a refusing scope answers `value` unchanged, always: it reports rather than
            // rewrites. safe to call with null, which is what a refusing position that has no
            // expression to hand back passes
            ExprNode *close(ExprNode *value);

        private:
            OwnershipPass &_pass;
            size_t _mark;
            const char *_action = nullptr;
            const char *_outcome = nullptr;
            bool _closed = false;
        };

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

        // the index into _frames of each enclosing loop body's frame, innermost last.
        //
        // this is the *bound* on a break's unwind, and the whole difference between it and a return's: a
        // `break` unwinds from the innermost frame down to **and including** _loop_frames.back() and no
        // further, because the frames outside the loop are still live on the other side of the branch.
        //
        // a vector and not a single index, so a labelled `break N` is _loop_frames[size() - N] the day it
        // is spelled, with the unwind loop unchanged. cleared in resolve_function *and* resolve_root: a
        // stale index from a previous body either reads out of range or clips an unwind to the wrong
        // depth, and _processed_functions means the wrong answer is never revisited
        std::vector<size_t> _loop_frames;

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

        // how many temporaries this body has minted, so their names are distinct. reset per body by
        // both entry points - see make_temporary for why they are numbered at all
        size_t _temporary_count = 0;

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

        // a synthesized local holding `init`, positioned at `site`. what the two binders above and
        // below share: one is a statement's worth of storage owned by the frame, the other an
        // expression's worth owned by a TemporaryBindExprNode, and the declaration is the same
        // declaration either way
        VarDeclNode &make_temporary(ExprNode *init, const TokenReference &site);

        // **answers the expression to use in place of `expr`**, which is `expr` itself for everything
        // except a member access that had to be given storage. every arm reseats the edges it owns, so
        // a replacement is observable - it returned void while nothing it visited could be replaced
        ExprNode *walk_expression(ExprNode *expr);

        // a **value** edge: the expression is read here, so a temporary requested anywhere below is
        // bound *around this edge* and destroyed once it has been read. walk plus flush, and the pair
        // is what keeps evaluation order honest - wrapping further out would move a temporary's
        // initializer ahead of everything to its left
        //
        // its counterpart is a plain walk_expression, which is what a **place** edge does: a member
        // base, an index base, `&`, a deref, `:$`. those are still addressing the temporary's storage,
        // so the request travels outward through them - exactly the set AST::place_root_of walks
        ExprNode *walk_value_edge(ExprNode *expr);

        // the nodes walked so far that need storage for their operand, innermost first. two owners, and
        // between them every way a value with no home is reached into: a **member access**, whose base is
        // the value (`$o->get()->tag`), and an **`&`**, whose operand is - which is how a *receiver* gets
        // one, since the parser addresses it (`$o->get()->size()`)
        //
        // a request rather than a rewrite: nothing is edited until a MaterializationScope closes, so the
        // positions that refuse one instead leave the tree exactly as it was written
        //
        // one queue for the whole walk, and the scopes index into it by mark - which is what makes
        // forwarding free: a position that opens no scope simply does not take anything off it
        std::vector<ExprNode *> _pending_temporaries;

        // **the one place a request is raised**, so the queue has a single writer and the three arms that
        // can reach into a value with no home - `$o->get()->tag`, `$o->get()->size()`, `f(41)` - all put
        // theirs on it the same way. *whether* to raise one stays with the arm, which is the only thing
        // that knows: a member base additionally needs somewhere to reach into, and a discarded chain
        // base needs destroying rather than addressing. a `wants` parameter here read as though this
        // owned that question too, and it never has
        void request_storage_for(ExprNode *owner);

        // **the operand edge a request means, as one answer rather than three.** reading the operand,
        // reseating it once its temporary exists, and naming it in a diagnostic are the same question,
        // and asking it at three sites is what let them drift: the naming arm enumerated one owner kind
        // and blind-cast the rest
        PendingEdge pending_edge(ExprNode *owner) const;

        // how a diagnostic names what the request was reaching into - `its member 'tag'`, or just `it`
        std::string describe_pending(ExprNode *owner) const;

        // MaterializationScope's two closings, and the only two ways a request ever leaves the queue.
        // private to the pass and reached only through the scope, so a position cannot bind or refuse
        // without having declared which it does

        // binds every request above `mark` into a TemporaryBindExprNode wrapping `value`, in binding
        // order, with the drops in reverse. answers `value` unchanged when there are none, which is
        // every edge in almost every program
        ExprNode *bind_pending_temporaries(ExprNode *value, size_t mark);

        // discards every request above `mark`, reporting each: the position wanted the temporary's
        // *address*, and an address into a value destroyed at the end of the statement is the one thing
        // binding one cannot make safe
        void refuse_pending_temporaries(size_t mark, const char *action, const char *outcome);

        // a local moved out of on one branch of an `if` but not the other. the chapter says the
        // variable is unset afterwards, which settles reading it - but not who destroys the value on
        // the branch that kept it. see the definition
        void report_conditional_move(const VarDeclNode *decl);

        // --- copy or move ---------------------------------------------------------------------

        // resolves one value-arrival site. `wanted` is the destination type and `param` the
        // parameter it is arriving at, or null when the destination is not one. answers the
        // expression to use in place of `expr` - the operand with the `mv` marker erased, or `expr`
        // unchanged
        //
        // an arrival is a value edge, so this is arrive_value wrapped in the same flush
        // walk_value_edge performs - and the flush is deliberately **last**. that ordering is what
        // makes a class-typed member read off a temporary retain-then-release with no arm of its own:
        // arrive_value sees a place and wraps it in a RetainExprNode through the ordinary copy rule,
        // and only then does the temporary that owns the storage close over it
        ExprNode *resolve_value_arrival(
            ExprNode *expr, const ValueType &wanted, const VarDeclNode *param, ValueDestination destination);

        // resolve_value_arrival's own body, without the flush. split out rather than inlined because it
        // has eight early returns and every one of them has to be flushed
        ExprNode *arrive_value(
            ExprNode *expr, const ValueType &wanted, const VarDeclNode *param, ValueDestination destination);

        // --- drops ----------------------------------------------------------------------------

        // appends the drop statements for `frame`'s live locals, innermost value first: reverse
        // declaration order, as the chapter specifies
        void collect_frame_drops(const Frame &frame, std::vector<NodeReference> &out);

        // **what a statement that leaves owes**: the drops of every frame from the innermost down to
        // `floor_frame`, inclusive. a `return` passes 0 - it leaves the function, so no frame outlives
        // it - and a `break` passes _loop_frames.back(). the *bound* is the whole difference between
        // the two, which is why it is a parameter and not a second walk.
        //
        // `out` is **rebuilt**, not appended to, so an unwind is derived rather than accumulated.
        // _processed_functions and _processed_roots mean a body is in fact walked at most once ever, so
        // nothing today arrives twice - but that is a guarantee two visited-sets make and not one this
        // can see, and the scope-exit append in walk_scope has no equivalent
        void collect_unwind(size_t floor_frame, std::vector<NodeReference> &out);

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

        // **a teardown reaches a const value.** `const` is a promise about what the *program* writes,
        // and a drop is not one of its writes: the storage is going away, and nothing that happens to
        // it afterwards is observable. refusing here would make `const` unusable for every type that
        // owns anything - a const local of one could not be declared at all - which is protecting
        // nothing.
        //
        // spelled as the **explicit** cast a user would have to write for the same narrowing, so
        // nothing is weakened by it: AST::TypeChecker validates implicit casts only, and every arrival
        // at a mutable borrow the *program* wrote still answers to AST::const_receiver_refusal.
        //
        // the address is taken here rather than left to emit_resolved_member_call, which would then
        // have a `ptr<const Foo>` and nothing to cast it from. a place that is already an address, or
        // one that is not const, is handed back untouched so that rule keeps its single owner
        ExprNode *receiver_for_teardown(ExprNode *place);

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

        // every class in the bundle whose payload needs tearing down gets its deinit, whether or not this
        // program happens to release one.
        //
        // synthesizing on demand alone is not sound across separate compilations, and the failure is
        // silent. `__eco_release_<T>` is emitted per unit with linkonce_odr linkage, and its body branches
        // on whether the class has a deinit - so a build that never released a `str::buf` produced a
        // thunk that decrements and frees, a build that did produced one that also tears the payload down,
        // and both claim the same ODR symbol. The linker keeps whichever it saw first and the program
        // leaks, with nothing anywhere to point at.
        //
        // so existence has to be a function of the *type* rather than of what the build did with it -
        // which class_needs_deinit already is. This makes the synthesis agree with it.
        //
        // deliberately **not** done for copy constructors, which are generated the same way and share the
        // same linkage: nothing else observes whether one exists. A copy constructor's body is field-wise
        // assignment derived from the properties, so two units that both need one write the same bytes,
        // and a unit that does not need one simply does not ask. The deinit is special only because the
        // release thunk reads the slot
        void synthesize_pending_class_deinits();

        // where a class layout was declared: the module, so a swept deinit lands somewhere deterministic
        // rather than in whichever file's walk happened to reach the type first, and the declaration node,
        // which is the only thing holding the name token to position it at - a ComplexType carries its
        // name as a string and has no token of its own
        struct TypeHome
        {
            Module *module = nullptr;
            TypeDeclNode *decl = nullptr;
        };

        // rebuilt per round, like Monomorphizer::_decl_module and for the same reason: declarations are
        // still being appended while the fixpoint runs
        std::unordered_map<const ComplexType *, TypeHome> _type_module;

        void build_type_module_map();

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
        // **answers the declaration**, which is what the caller goes on to call. that is one asking of
        // the copy-constructor lookup rather than "publish it, then read the slot back" - the arm that
        // gets here has already classified the type, and re-deriving what it just decided is what A24
        // took out of this ladder
        //
        // deliberately **not** registered in AST::FunctionRegistry, unlike the parser's field-wise
        // constructor. that registry is read only while parsing, and a call site is resolved as it is
        // parsed - so a declaration created inside this fixpoint arrives after every written call was
        // already resolved or already reported. it would make `Pair($p)` no more callable than it is
        // now, and would put a per-instantiation declaration into a name-keyed overload set the
        // parser owns. `$q = $p` is the spelling
        FunctionDeclNode *ensure_copy_constructor(const ValueType &type, const TokenReference &site);

        // **there is no copy of this value, and this is what the author is told.** two wordings, and
        // which one applies is not a property of the type: a source that names no variable has no `mv`
        // to suggest, because `mv $doc->body` is rejected too
        //
        // split out of arrive_value's CopyKind switch so the t_none arm reads as one arm beside the
        // others rather than as fifteen lines of formatting
        void reject_uncopyable(
            ExprNode *expr, const ValueType &wanted, const VarDeclNode *source, ValueDestination destination);

        // declarations synthesized this round - class deinits and copy constructors - appended to the
        // file root after the walk rather than during it, since resolve_function is iterating those
        // children. one list because it is one mechanism: whatever lands here is emitted by codegen
        // and walked by the next round like any other declaration
        std::vector<FunctionDeclNode *> _pending_declarations;
    };
};

#endif
