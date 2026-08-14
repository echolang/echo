#ifndef ASTOWNERSHIP_H
#define ASTOWNERSHIP_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTControlFlow.h"
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
    class StaticPropertyExprNode;
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

    // where a value is arriving. two of the five take a place as an implicit move, a third takes one
    // where the place is provably dead afterwards; at the other two a place is always a copy:
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
    //  - **t_argument** is the conditional one, and the condition is a proof rather than a
    //    destination: a by-value parameter handed a local the enclosing `return` was about to destroy
    //    anyway takes it over, because `return f($x)` is `return $x` with a call in between and the
    //    two must not disagree about who ends `$x`. AST::handover_reads_in is the proof, and it is
    //    deliberately not part of this enum - what a destination *does* with a value is a property of
    //    the position, and whether the source is still wanted afterwards is a property of the body.
    //    **everywhere else an argument is a copy**, so the caller keeps its reference and the object
    //    outlives the call
    enum class ValueDestination
    {
        t_declaration,
        t_assignment,
        t_initialization,
        t_argument,
        t_return,
    };

    // single ownership, as book/concept/ownership_and_moving.md specifies it: one owner per value,
    // destroyed exactly once when that owner's scope ends, `mv` to hand it elsewhere.
    //
    // Three jobs in one tree walk, because they answer each other:
    //
    //  - **copy or move.** at each place a value arrives - a declaration's initializer, an
    //    assignment, an initialization, a call argument, a `return` - a *place* source is a copy and a
    //    non-place source is already a move. For an owning type the copy is rejected (below) and `mv`
    //    marks the source moved-from.
    //
    //    `return $local` is a move with no `mv`, and that is the rule the feature rests on: a
    //    constructor's `$this` is a body-local with an implicit `return $this`, so without it
    //    `$a = Buffer(...)` frees the buffer twice
    //  - **moved state.** a set of moved-from declarations carried down the walk and merged by union
    //    at a branch, so a move on one arm of an `if` leaves the variable unset after it. Reading a
    //    moved local is a located error rather than a runtime surprise
    //  - **drops.** at every scope end and before every `return`, a destructor call per live local in
    //    reverse declaration order, skipping whatever was moved out
    //
    // **every drop is an ordinary FunctionCallExprNode in the tree**, receiver `&$local`. That is the
    // same choice AST::PointerAdjuster makes about derefs, and it buys the same three things: codegen
    // needs no scope-exit machinery, -ar shows the drops, and AST::TypeChecker validates what this
    // pass inserted. (-a is the tree as *parsed*, before this pass runs at all.)
    //
    // **a copy is a call to the type's copy constructor**, when it has one.
    //
    // The member-wise deep copy the chapter describes is not what happens, and never was. That
    // recursion has no bottom: every owning type bottoms out at a raw `ptr<uint8>` the type system
    // knows nothing about, so copying it member-wise leaves two owners freeing one allocation. The
    // type holding the pointer is the one that knows, and a constructor taking a borrow of its own
    // type is it saying so.
    //
    // Two things fall out of *recognising* that constructor rather than spelling a new form for it
    // (AST::is_copy_constructor). `$b = $a` and `Foo($a)` are one declaration, so they cannot drift.
    // And it is honoured whether or not the type owns anything, so what a copy means never depends on
    // whether a destructor happens to be declared.
    //
    // With no copy constructor, an implicit copy of an *owning* type is still a located error naming
    // `mv`, a borrow, and now the third option.
    //
    // **runs inside the monomorphizer's fixpoint**, beside the "re-derive a declaration's type from
    // its initializer" step. It has to, for two reasons. Whether a `T $x` needs destroying is unknown
    // until substitution. And inserting a drop *creates* a generic call site - a `Box<int32>` local's
    // drop names the template's destructor, and the next round instantiates it through the ordinary
    // path, which is why no instantiation logic lives here
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
        // There are three answers, and only two of them are a scope. That is the point:
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
        // Spelling "forward" as the absence of a scope is deliberate. It is the overwhelmingly common
        // case - every `&`, `->`, `[…]`, `:$` and `?->` in the program - so a walker arm says nothing
        // and gets the right answer, while a position that *does* decide has to say which decision it
        // is making.
        //
        // The two constructors are the two decisions, and neither can be written without its reason:
        // no refusal without wording, no bind without an expression to hang the drops on.
        //
        // A **frame** lifetime is deliberately not one of these. Binding a value to the enclosing
        // frame is a rewrite of the *statement* rather than of an expression edge - the statement
        // becomes the declaration - so bind_discarded_temporary owns it, sharing make_temporary and
        // nothing else. One mint, two shapes
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

        // one enclosing loop body, and what its two exits carry out of it.
        //
        // `frame_floor` is the index into _frames of the body's own frame. this is the *bound* on a
        // break's unwind, and the whole difference between it and a return's: a `break` unwinds from the
        // innermost frame down to **and including** frame_floor and no further, because the frames
        // outside the loop are still live on the other side of the branch.
        //
        // the two sets are the moved state each exit *reaches its destination with*. a branch that
        // leaves does not reach the join after the statement it sits in - an `if` arm ending in `break`
        // continues after the loop, not after the `if` - so its moves are recorded here and merged by
        // whoever owns that destination. without them an exit-aware `if` merge would drop them, which is
        // a use-after-move with no diagnostic rather than an over-approximation
        struct LoopFrame
        {
            size_t frame_floor = 0;

            std::unordered_set<const VarDeclNode *> break_moved;
            std::unordered_set<const VarDeclNode *> continue_moved;
        };

        // the enclosing loop bodies, innermost last. a vector and not a single frame, so a labelled
        // `break N` is _loop_frames[size() - N] the day it is spelled, with the unwind loop unchanged.
        // cleared in resolve_function *and* resolve_root: a stale index from a previous body either reads
        // out of range or clips an unwind to the wrong depth, and _processed_functions means the wrong
        // answer is never revisited
        std::vector<LoopFrame> _loop_frames;

        // declarations whose value has been moved out. a moved local is neither readable nor
        // dropped - "its destructor travelled with the value"
        std::unordered_set<const VarDeclNode *> _moved;

        // locals moved inside a branch that did not certainly run. read a second time, so the
        // diagnostic can say "may have been moved" rather than claiming it definitely was
        std::unordered_set<const VarDeclNode *> _maybe_moved;

        // **the reads at which this body hands an owner over**, from AST::handover_reads_in - answered
        // once per body, before the walk, and looked up by arrive_value at every by-value argument.
        //
        // before the walk rather than during it, because the question is about what comes *after* the
        // arrival and this walk only ever knows what came before. the nodes it names are the ones the
        // parser wrote, and the walk replaces none of them: a read is a VarRefNode on the way in and
        // on the way out, whatever this pass wraps around it
        std::unordered_set<const ExprNode *> _handover_reads;

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
        //
        // **hands back how far control goes when it leaves**, which is the answer it already had to
        // compute for the drops above. asked of AST::scope_exit_kind, which is an unmemoized walk that
        // recurses into nested branches - so the arms that need it for a scope this walked (both `if`
        // arms, a loop body) read it from here rather than asking again, and the pass keeps to one such
        // walk per scope
        ExitKind walk_scope(ScopeNode &scope);

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

        // **the one loop walk**, shared by `while` and `for`. `step` is null for a `while`, whose
        // condition *is* its step - and for the `foreach` that lowers into one, whose advance lives
        // there by design. it is walked inside the loop's frame and after the body, because that is
        // when it runs: on the fall-through, and on every `continue`
        //
        // the condition is a *reference* to the edge, because walking one may replace it - a temporary
        // materialized in a loop's condition is bound and destroyed inside the block that evaluates it
        void walk_loop(ExprNode *&condition, ScopeNode *body, ScopeNode *step);

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
            ExprNode *expr,
            const ValueType &wanted,
            const VarDeclNode *param,
            ValueDestination destination
        );

        // resolve_value_arrival's own body, without the flush. split out rather than inlined because it
        // has eight early returns and every one of them has to be flushed
        ExprNode *arrive_value(
            ExprNode *expr,
            const ValueType &wanted,
            const VarDeclNode *param,
            ValueDestination destination
        );

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

        // destroying a value of `type` at `root`->`path`: one call to whatever ensure_deinit answers for
        // it, or one release when the value is a handle rather than the thing.
        //
        // **the teardown is not written here.** it used to be - the destructor and then a drop per owning
        // property, inlined into whichever scope held the value - and the member accesses that took were
        // the compiler reaching inside a type from outside it, which is a `private` refusal against a line
        // nobody wrote. what a synthesized declaration could not answer *in the parser* is whether a
        // generic property needs destroying, and this pass runs after instantiation, so it can.
        //
        // `path` is the member path from `root` down to the value being destroyed, and it is one
        // vector pushed and popped in step with the recursion rather than a copy per property: a
        // deep struct graph would otherwise reallocate and re-copy the whole path at every level
        void emit_drop(
            VarDeclNode *root,
            std::vector<std::string> &path,
            const ValueType &type,
            std::vector<NodeReference> &out);

        // **the same taxonomy, over storage that is already a place.** the spelling above builds
        // `$root->a->b` and hands it here; a static property's global has no root declaration to build
        // one from and hands its own access node over instead.
        //
        // lifted rather than duplicated because the arms *are* the language rule - a callable, an
        // interface and a weak owe one release, a class owes one release and a deinit for later, and a
        // struct owes a call - and a second copy of them would be a second answer to what ending a
        // value means
        void emit_drop_of_place(
            ExprNode *place,
            const ValueType &type,
            const TokenReference &at,
            std::vector<NodeReference> &out);

        // `<base>-><name>` - one step of a path, and the one place this pass mints a member access. both
        // spellings below go through it, so anything a synthesized access later has to carry is set once
        ExprNode *member_place(ExprNode *base, const std::string &name, const TokenReference &at);

        // `<place>->__value` - the payload of a tagged optional, as a place. one member access, because the
        // pair is a layout and `__value` is one of its two properties
        ExprNode *optional_payload_place(ExprNode *optional_place, const TokenReference &at);

        // a fresh `$root->a->b` place for one drop. rebuilt per drop rather than shared, so no node
        // sits in the tree twice
        ExprNode *make_place(VarDeclNode *root, const std::vector<std::string> &path);

        // a resolved call to `callee` with `place` as its borrow receiver, positioned at `at`
        //
        // The receiver is addressed here, exactly as the parser addresses a method's: the parameter is
        // the borrow `Foo&`, and a value ranked against it would be no fit at all. Unless the place
        // already *is* that address - the `$this` of a synthesized class deinit, declared `Foo&`,
        // which would otherwise be handed a ptr<ptr<Foo>>.
        //
        // `decl` is set directly rather than resolved. There is no name to look up and no overload set
        // to search. For an instantiation it is the *template's* declaration, and the monomorphizer's
        // next round binds the owner's parameters from the receiver and rewires the call to the
        // instance - which is the whole reason this pass runs inside that fixpoint.
        //
        // Shared by the two things this pass inserts that call a member, a drop and a copy. They differ
        // only in which declaration they name and where the place comes from
        FunctionCallExprNode &emit_resolved_member_call(
            FunctionDeclNode *callee,
            const TokenReference &at,
            ExprNode *place
        );

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

        // a call to `callee` tearing down the value at `root`->`path`. the receiver is the address of the
        // place - except when the place already *is* that address, which is a deinit's `$this`.
        //
        // two askers, naming the two callees ensure_deinit can answer with: the drop site, and
        // emit_destructor_call below on behalf of a body being built
        void emit_teardown_call(
            FunctionDeclNode *callee,
            ExprNode *place,
            const TokenReference &at,
            std::vector<NodeReference> &out);

        // the destructor call for one value, when its type declares one
        void emit_destructor_call(
            VarDeclNode *root,
            const std::vector<std::string> &path,
            const ComplexType *ct,
            std::vector<NodeReference> &out);

        // `if ($tag_root->__has) { <body> }` - the one thing a tagged optional's teardown and its copy do
        // that a struct's do not, and the *only* thing.
        //
        // minted here rather than at each drop and copy site, so it exists once per type inside the body
        // that type owns - which is the whole point of the pair being a layout. shared by the two bodies
        // because their `if` has to be the same `if`: build_deinit wraps everything it collected and reads
        // the tag off `$this`, ensure_copy_constructor wraps the payload's write alone and reads it off
        // `$other`, and those two differences are the whole of what they do not share
        IfStatementNode &branch_when_present(
            VarDeclNode *tag_root,
            ScopeNode *body,
            const TokenReference &at
        );

        // `if ($tag_root->__tag == <discriminant>) { <body> }` - the same thing one case of an enum
        // wide, and the reason branch_when_present above is not the only shape: an optional has one
        // payload and asks a `bool`, an enum has one per case and asks which.
        //
        // this one mints a comparison where that one reads a place, and the comparison is over the
        // discriminant's own primitive - so AST::binary_has_builtin_meaning answers it directly and no
        // declared `operator ==` is consulted. an enum whose author declared one is therefore torn down
        // by the tag it actually holds rather than by what they said equality means
        IfStatementNode &branch_when_case(
            VarDeclNode *tag_root,
            const ComplexType *ct,
            int64_t discriminant,
            ScopeNode *body,
            const TokenReference &at
        );

        // the shared half of the two above: the branch itself, with no else arm.
        //
        // no else because neither caller has one - an absent optional owes nothing and a case that is
        // not live owes nothing - and because AST::scope_exit_kind reads a branch with no else as
        // leaving nothing, which is what keeps both of them out of every control-flow rule
        IfStatementNode &branch_on(
            ExprNode *condition,
            ScopeNode *body,
            const TokenReference &at
        );

        // each case of `ct` whose payload owns something, as one guarded group per case.
        //
        // **the enum half of emit_property_drops**, and a different walk rather than a filter over
        // that one: a struct's properties are all live at once and an enum's are live one case at a
        // time, so what changes is not which properties are dropped but how many branches they are
        // spread over. reverse declaration order *within* a case, as there
        void emit_enum_case_drops(
            VarDeclNode *root,
            const ComplexType *ct,
            std::vector<NodeReference> &out,
            const TokenReference &at);

        // each property of `ct` that needs destroying, in reverse declaration order.
        //
        // **called from build_deinit and nowhere else**, which is what keeps the member accesses it mints
        // inside a body whose owner is `ct`. it is the only thing in this pass that reaches `make_place`
        // with a non-empty path
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
        // `site` is a real line: the type's own name for a deinit, the copy that asked for a copy
        // constructor. a synthesized declaration at line 0 gives every diagnostic raised inside its body
        // nowhere to point, and gives a `-g` build a subprogram nobody can step into
        FunctionDeclNode &begin_synthesized_decl(const std::string &name, const TokenReference &site);

        // a single non-nullable borrow parameter, which is what both synthesized declarations take -
        // a deinit's `$this` and a copy constructor's `$other`. `Foo&` rather than `Foo`: a by-value
        // parameter of an owning type is an owner, and neither of them may own its argument
        VarDeclNode &add_borrow_parameter(
            FunctionDeclNode &decl,
            const std::string &name,
            const ValueType &borrowed,
            const TokenReference &site
        );

        // hands a finished declaration to the file root it was synthesized into, and marks the round
        // changed so the next one walks its body
        //
        // through `_pending_declarations` rather than add_funcdecl directly: run_round is iterating
        // the very children this appends to. codegen emits a body only for a declaration that is one
        // of them, so a synthesizer that skips this step emits a `declare` nobody defines
        void publish_synthesized_decl(FunctionDeclNode &decl);

        // --- teardown ---------------------------------------------------------------------------

        // **the one function that tears a value of this type down**, or null when it owes no teardown.
        //
        // A class reaches it from its release thunk at the moment the strong count hits zero; a struct
        // reaches it from the drop this pass wrote at the end of the value's scope. One question, because
        // there is one teardown: what a class destroys at zero and what a struct destroys at a scope end
        // can never disagree, since one piece of code decides both.
        //
        // Three answers, and the middle one is the interesting one:
        //
        //   - null, when AST::needs_deinit says the value owns nothing. For a class that is a release
        //     which decrements and frees, and codegen skips the call it has nothing to make.
        //   - **the declared destructor, when it is the whole teardown** - a struct with no owning
        //     property. Nothing is synthesized and nothing fills the slot: the drop site calls what the
        //     author wrote, which is what it has always done. `mem::buffer<T>` is the live example, and
        //     every array, string and map holds one. A *class* is excluded from this arm, because its end
        //     of the wire is codegen reading the slot, which needs a concrete symbol - and for an
        //     instantiation AST::find_destructor answers the template's declaration, which has none.
        //   - a synthesized `$deinit` otherwise, built by build_deinit below.
        //
        // Idempotent through the slot, so it is safe to ask at every drop site, and it answers "not yet"
        // rather than guessing for a layout whose properties are not filled in - see build_deinit.
        //
        // `at` is the teardown that asked, and is used only when the type has no home to be written at -
        // absent from the sweep, which is asking about types nothing in the program has torn down yet
        FunctionDeclNode *ensure_deinit(const ValueType &type, std::optional<TokenReference> at);

        // **the function that seats a static property's value, and the one that ends it.**
        //
        // a static's initializer is written outside every body: it sits on the declaration, in a type
        // declaration, where no frame exists and no pass that decides ownership ever walks. so the
        // whole of what this does is *give it a body* - `Type::$x = <what was written>;` in a real
        // scope, published like any other synthesized declaration - and from there every rule that
        // matters was decided by the passes that already know how: the copy taxonomy, the temporary
        // materialization, the drop of whatever the initializer built and did not hand over.
        //
        // that is the reason this is here rather than in codegen. an initializer emitted straight into
        // the init function would be an expression nobody had walked, so an owning value would be
        // seated with no retain and torn down twice - or not at all
        //
        // memoized on (type, index), and idempotent: every access to one static asks, and the first
        // one to be reached is the author. answers null while the owner is a template, which is a
        // *not yet* on the same terms ensure_deinit's guards are
        void ensure_static_init(StaticPropertyExprNode &node);

        // what a static property's storage owes: the function that fills it, and the one that ends it.
        // both may legitimately be null - a static with no initializer needs no filling, and one whose
        // type owns nothing needs no ending - which is why this is a pair rather than a pointer
        struct StaticInit
        {
            FunctionDeclNode *init = nullptr;
            FunctionDeclNode *deinit = nullptr;
        };

        // what ensure_static_init already answered for this (type, index), so the second access to a
        // static does not build a second body for it
        std::map<std::pair<const ComplexType *, size_t>, StaticInit> _static_inits;

        // builds the body: the type's own destructor first, then each owning property in reverse
        // declaration order, out of emit_destructor_call and emit_property_drops.
        //
        // **it is the only caller of either**, and that is the whole of the rule: those two mint
        // member accesses over the value's properties, and a member access minted anywhere other than
        // inside a body the type owns is refused by `private` - which made a struct with a private owning
        // property unusable at every use site while a class, whose teardown was already a body like this
        // one, was fine.
        //
        // `site` is where the declaration is written, which ensure_deinit resolves to the type's own name
        // token wherever the type has a home. A deinit is shared by every teardown of the type, so the
        // first teardown that happened to need it is the worst possible author: the body's DISubprogram
        // would name the owner's file and a line from somewhere else entirely
        FunctionDeclNode *build_deinit(ComplexType &type, const TokenReference &site);

        // every **class** in the bundle whose payload needs tearing down gets its deinit, whether or not
        // this program happens to release one.
        //
        // Synthesizing on demand alone is not sound across separate compilations, and the failure is
        // silent. `__eco_release_<T>` is emitted per unit with linkonce_odr linkage, and its body
        // branches on whether the class has a deinit. So a build that never released a `str::buf`
        // produced a thunk that decrements and frees, a build that did produced one that also tears the
        // payload down, and both claim the same ODR symbol. The linker keeps whichever it saw first,
        // and the program leaks with nothing anywhere to point at.
        //
        // So existence has to be a function of the *type* rather than of what the build did with it,
        // which AST::needs_deinit already is. This makes the synthesis agree with it.
        //
        // Deliberately **not** done for a struct, nor for copy constructors, which are generated the same
        // way and share the same linkage - because nothing else observes whether either exists. A struct's
        // deinit is reached from a call the ownership pass writes at the drop site, and that call and the
        // body are minted together; a copy constructor's body is field-wise assignment derived from the
        // properties. In both cases two units that need one write the same bytes and a unit that does not
        // simply does not ask. The class deinit is special only because the release thunk reads the slot
        void synthesize_pending_class_deinits();

        // where a type was declared: the module, so a synthesized deinit lands somewhere deterministic
        // rather than in whichever file's walk happened to reach the type first, and the declaration node,
        // which is the only thing holding the name token to position it at - a ComplexType carries its
        // name as a string and has no token of its own
        struct TypeHome
        {
            Module *module = nullptr;
            File *file = nullptr;
            TypeDeclNode *decl = nullptr;
        };

        // rebuilt per round, like Monomorphizer::_decl_module and for the same reason: declarations are
        // still being appended while the fixpoint runs
        std::unordered_map<const ComplexType *, TypeHome> _type_module;

        void build_type_module_map();

        // **writes a synthesized body at its type rather than at the site that asked for it**, for the
        // duration of a scope.
        //
        // every `linkonce_odr` body this pass mints owes this: a deinit, a copy constructor and a
        // static's initializer are shared by every use of the type, so the first use to need one is the
        // worst possible author. the body's DISubprogram would take its file from the owner and its line
        // from a virtual token in another file, and its call nodes would land in whichever module's walk
        // reached the type first - which build_function_maps' arena sweep turns into a different
        // function order in that module's object. two units emitting different bytes for one symbol is
        // unsound rather than untidy, which is why this is the shape it is.
        //
        // **scoped rather than a swap-and-restore pair**: an early return between them leaves the pass
        // writing into another module's file, and nothing downstream says so
        class TypeHomeScope
        {
        public:
            TypeHomeScope(OwnershipPass &pass, const ComplexType *ct);
            ~TypeHomeScope();

            TypeHomeScope(const TypeHomeScope &) = delete;
            TypeHomeScope &operator=(const TypeHomeScope &) = delete;

            // the type's recorded home, or null when nothing placed it - a compiler-minted anonymous
            // layout, or a module whose file the body pass never built. the caller decides what that
            // means for it: a deinit has nowhere to be written and gives up, a static's initializer is
            // positioned on its own property declaration and carries on
            const TypeHome *home() const { return _home; }

        private:
            OwnershipPass &_pass;
            Module *_previous_module;
            File *_previous_file;
            const TypeHome *_home = nullptr;
        };

        // --- copies ----------------------------------------------------------------------------

        // the copy constructor for a struct whose owning properties are all classes, transitively:
        // the body its author would have written, which is a field-wise assignment and nothing else
        //
        // No retain appears in what this builds. `$this->a = $other->a` is an ordinary assignment, so
        // the next round's walk reaches it through resolve_value_arrival and inserts the retain there -
        // the same arm a hand-written copy constructor's body goes through.
        //
        // A property that is a struct with a copy of its own gets a resolved call to it instead, and
        // one that needs a synthesized copy asks for its own here. That is where the recursion lives.
        //
        // Synthesized on demand at the first copy that needs it, and per *concrete* type rather than
        // per template - because whether the compiler can write the body at all depends on the property
        // types, and `Box<Handle>` can while `Box<Buffer>` cannot. AST::copy_is_synthesizable
        // (ASTCopy.h) is the rule, and it declines a type that already has a written one.
        //
        // **answers the declaration**, which is what the caller goes on to call. That is one asking of
        // the copy-constructor lookup rather than "publish it, then read the slot back". The arm that
        // gets here has already classified the type, and re-deriving what it just decided is what A24
        // took out of this ladder.
        //
        // Deliberately **not** registered in AST::FunctionRegistry, unlike the parser's field-wise
        // constructor. That registry is read only while parsing, and a call site is resolved as it is
        // parsed, so a declaration created inside this fixpoint arrives after every written call was
        // already resolved or already reported. Registering it would make `Pair($p)` no more callable
        // than it is now, and would put a per-instantiation declaration into a name-keyed overload set
        // the parser owns. `$q = $p` is the spelling
        FunctionDeclNode *ensure_copy_constructor(const ValueType &type, const TokenReference &site);

        // **there is no copy of this value, and this is what the author is told.** two wordings, and
        // which one applies is not a property of the type: a source that names no variable has no `mv`
        // to suggest, because `mv $doc->body` is rejected too
        //
        // split out of arrive_value's CopyKind switch so the t_none arm reads as one arm beside the
        // others rather than as fifteen lines of formatting
        void reject_uncopyable(
            ExprNode *expr, const ValueType &wanted, const VarDeclNode *source, ValueDestination destination);

        // a declaration synthesized this round, and the file root it belongs to. two fields because the
        // second is not the walk's own file: a deinit is built into the file that declares its type, which
        // may be a module the walk is nowhere near
        struct PendingDecl
        {
            FunctionDeclNode *decl = nullptr;
            File *file = nullptr;
        };

        // declarations synthesized this round - deinits and copy constructors - appended to their file
        // roots **once, at the end of the round** rather than during the walk, since resolve_function is
        // iterating those children. one list drained in one place because it is one mechanism: whatever
        // lands here is emitted by codegen and walked by the next round like any other declaration, and a
        // second channel is what would let a synthesizer append into a root something is iterating
        std::vector<PendingDecl> _pending_declarations;
    };
};

#endif
