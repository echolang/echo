#ifndef ASTOWNERSHIP_H
#define ASTOWNERSHIP_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTControlFlow.h"
#include "AST/ASTValueType.h"

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
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
    class AssignNode;
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

    // where a value is arriving. a place is a copy except:
    //   - t_return: the local's scope ends here, so it moves with no `mv`
    //   - t_initialization: the synthesized field-wise constructor (a user ctor writes `mv`)
    //   - t_argument: only when handover_reads_in proves the enclosing `return` was about to
    //     destroy it anyway (`return f($x)`). everywhere else an argument is a copy
    enum class ValueDestination
    {
        t_declaration,
        t_assignment,
        t_initialization,
        t_argument,
        t_return,
    };

    // one owner per value, destroyed at scope end, `mv` to hand it elsewhere.
    // one walk: copy vs move, moved-from state, drops as ordinary calls in the tree.
    // `return $local` moves with no `mv` (a constructor's `$this` is a body-local).
    // a copy is the type's copy constructor; without one, an owning copy is a located error.
    // inside the fixpoint: a drop of `Box<int32>` creates a generic call site. a body is walked once
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
        // what an enclosing position does with storage requests raised beneath it:
        //   - bind: the position reads a value, so it outlives every request below
        //   - refuse: no lifetime is long enough (needs wording)
        //   - forward: no scope at all (a call, every place edge). a walker arm that
        //     says nothing forwards, which is the common case
        class MaterializationScope
        {
        public:
            // **does anything read the bound value after this statement?**
            //
            // only asked by the binding form, and only to decide one thing: whether a *pointer* the
            // bound value hands back could still be read once the temporaries under it are gone.
            // a discarded statement expression reads nothing, so a borrow it produced is no
            // hazard even when the statement's own type happens to be a pointer
            enum class BoundValue
            {
                t_kept,
                t_discarded,
            };

            // the binding form. `pass` outlives it by construction: every scope is a local of a
            // method on the pass
            explicit MaterializationScope(OwnershipPass &pass, BoundValue kept = BoundValue::t_kept);

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
            BoundValue _kept = BoundValue::t_kept;
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

        // **the facts this body has accumulated so far**, snapshotted and restored as one value at
        // every exclusive arm, and recorded whole on a loop exit. three sets that travelled
        // separately is how an `if` arm's initialization was visible to the other arm, and how an
        // init that left by `break` vanished after the loop. a fourth fact is a field here, or it
        // does not exist
        struct FlowFacts
        {
            // declarations whose value has been moved out. a moved local is neither readable nor
            // dropped - "its destructor travelled with the value"
            std::unordered_set<const VarDeclNode *> moved;

            // locals moved inside a branch that did not certainly run. read a second time, so the
            // diagnostic can say "may have been moved" rather than claiming it definitely was
            std::unordered_set<const VarDeclNode *> maybe_moved;

            // the storage this body has already *initialized* - a declaration and the member names
            // below it, as one key. an initialization owes the old value no teardown because there
            // is no old value, and this is what keeps that claim honest: a second write to the same
            // owning field would leak what the first one built, with nothing further down able to
            // notice
            std::unordered_set<std::string> initialized;

            void merge(const FlowFacts &other)
            {
                moved.insert(other.moved.begin(), other.moved.end());
                maybe_moved.insert(other.maybe_moved.begin(), other.maybe_moved.end());
                initialized.insert(other.initialized.begin(), other.initialized.end());
            }
        };

        // one enclosing loop body, and what its two exits carry out of it.
        //
        // `frame_floor` is the index into _frames of the body's own frame. this is the *bound* on a
        // break's unwind, and the whole difference between it and a return's: a `break` unwinds from the
        // innermost frame down to **and including** frame_floor and no further, because the frames
        // outside the loop are still live on the other side of the branch.
        //
        // the two FlowFacts are what each exit *reaches its destination with*. a branch that leaves
        // does not reach the join after the statement it sits in - an `if` arm ending in `break`
        // continues after the loop, not after the `if` - so its facts are recorded here and merged by
        // whoever owns that destination. without them an exit-aware `if` merge would drop them: a
        // move is a use-after-move with no diagnostic, an initialization is a second write after the
        // loop that leaks what the break built
        struct LoopFrame
        {
            size_t frame_floor = 0;

            FlowFacts break_facts;
            FlowFacts continue_facts;
        };

        // the enclosing loop bodies, innermost last. a vector and not a single frame, so a labelled
        // `break N` is _loop_frames[size() - N] the day it is spelled, with the unwind loop unchanged.
        // cleared in resolve_function *and* resolve_root: a stale index from a previous body either reads
        // out of range or clips an unwind to the wrong depth, and RegionState::t_owned means the wrong
        // answer is never revisited
        std::vector<LoopFrame> _loop_frames;

        FlowFacts _flow;

        // **the reads at which this body hands an owner over**, from AST::handover_reads_in - answered
        // once per body, before the walk, and looked up by arrive_value at every by-value argument.
        //
        // before the walk rather than during it, because the question is about what comes *after* the
        // arrival and this walk only ever knows what came before. the nodes it names are the ones the
        // parser wrote, and the walk replaces none of them: a read is a VarRefNode on the way in and
        // on the way out, whatever this pass wraps around it
        std::unordered_set<const ExprNode *> _handover_reads;

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

        // parameter defaults are evaluated at the call site as arguments, so
        // `const string& $s = ''` is a borrow argument. this scope binds the slot
        // on the original; call-site clones are arguments of the call that used them
        void walk_parameter_defaults(const std::vector<VarDeclNode *> &params);

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

        // **the one exclusive-arm join**, which `if` and `match` both go through. each joining arm is
        // a path that reaches the code after the statement; a leaving arm has already recorded what
        // it carries onto a return's unwind or the enclosing loop's frame
        //
        // 0 joining: the tail is unreachable, `_flow` stays `before`. 1 joining: that arm *is* the
        // join. several: `initialized` and `maybe_moved` union, `moved` only where every arm moved it
        void join_exclusive(
            const FlowFacts &before,
            std::vector<FlowFacts> joining,
            const std::string &remedy = "Move it on every branch, or after the 'if'."
        );

        // join a skipped `&&` / `||` / `??` / `?->` operand so a never-returning
        // rhs cannot look like a second path
        template<typename Walk>
        ExprNode *walk_skippable_operand(ExprNode *operand, const std::string &op, Walk &&walk);

        // binds a discarded owning value to a synthesized local of the enclosing frame, so the scope
        // destroys it. the frame's ordinary reverse-order drop then covers it with no special case
        VarDeclNode &bind_discarded_temporary(ExprNode *expr);

        // a synthesized local holding `init`, positioned at `site`. what the two binders above and
        // below share: one is a statement's worth of storage owned by the frame, the other an
        // expression's worth owned by a TemporaryBindExprNode, and the declaration is the same
        // declaration either way
        //
        // `prefix` is the spelling between `$__` and the per-body counter, so a RAST golden can name
        // `$__elemN` without a second counter. the default is what every other mint already prints
        VarDeclNode &make_temporary(ExprNode *init, const TokenReference &site, const char *prefix = "temp");

        // binds the target's address to a local when the place reaches through a container element
        // or a `T[N]` index, so the teardown below and the store in gen_assign address it once
        // between them. a no-op for every other place shape - a member path is a route the teardown
        // can simply rebuild. the target has already been walked; this must not walk the bind
        void bind_indexed_target(AssignNode &assign);

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
        //
        // `kept` has exactly one caller that does not take the default, the statement-that-is-an-
        // expression arm of walk_statement. see MaterializationScope::BoundValue
        ExprNode *walk_value_edge(
            ExprNode *expr,
            MaterializationScope::BoundValue kept = MaterializationScope::BoundValue::t_kept
        );

        // a borrowed place hanging on a t_materializable form: `?->` continuation, `??`
        // rhs, mixed match arm. hang the copy on the producing place; a computed value
        // is already an owner and is only walked
        ExprNode *arrive_computed_place(ExprNode *expr);

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
        //
        // `kept` decides one arm and nothing else - the escape guard over a pointer-typed value. see
        // MaterializationScope::BoundValue
        ExprNode *bind_pending_temporaries(
            ExprNode *value,
            size_t mark,
            MaterializationScope::BoundValue kept
        );

        // discards every request above `mark`, reporting each: the position wanted the temporary's
        // *address*, and an address into a value destroyed at the end of the statement is the one thing
        // binding one cannot make safe
        void refuse_pending_temporaries(size_t mark, const char *action, const char *outcome);

        // a local moved out of on one branch of an `if` but not the other. the chapter says the
        // variable is unset afterwards, which settles reading it - but not who destroys the value on
        // the branch that kept it. see the definition
        void report_conditional_move(const VarDeclNode *decl, const std::string &remedy);

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
        // RegionState::t_owned means a body is in fact walked at most once ever, so nothing today
        // arrives twice - but that is a guarantee the region makes and not one this can see, and the
        // scope-exit append in walk_scope has no equivalent
        void collect_unwind(size_t floor_frame, std::vector<NodeReference> &out);

        // destroying a value of `type` at `root`->`path`: one call to whatever ensure_deinit answers for
        // it, or one release when the value is a handle rather than the thing.
        //
        // **the teardown is not written here.** inlining a destructor and a drop per owning property
        // into the scope that held the value would mint member accesses from outside the type,
        // which is a `private` refusal against a line nobody wrote. what a synthesized declaration
        // could not answer *in the parser* is whether a generic property needs destroying, and this
        // pass runs after instantiation, so it can.
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

        // `$i = 0; while ($i < n) { body; $i = $i + 1 }` or the reverse counted form drops use.
        // `fill` writes the per-element work into the loop body; the counter step is this
        // helper's, so the two T[N] loops cannot drift
        void emit_counted_loop(
            uint64_t length,
            bool reverse,
            const TokenReference &at,
            ScopeNode &into,
            const std::function<void(VarDeclNode &index, ScopeNode &body)> &fill);

        // copy or drop each element of a `T[N]` place. N is bound; the loop is ordinary Echo
        // statements so the per-element copy and drop reuse the rest of this pass
        void emit_inline_array_copies(
            ExprNode *dst,
            ExprNode *src,
            const ValueType &array_type,
            const TokenReference &at,
            ScopeNode &into);

        void emit_inline_array_drops(
            ExprNode *place,
            const ValueType &array_type,
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

        // the teardown for this type, or null when it owns nothing. a struct with only a declared
        // destructor uses that; everything else gets a synthesized `$deinit`. idempotent through the
        // slot. `at` is the drop site, used only when the type has no home
        FunctionDeclNode *ensure_deinit(const ValueType &type, std::optional<TokenReference> at);

        // give a static property a real body (`Type::$x = <written>;`) so ownership walks it. memoized
        // on (type, index); null while the owner is still a template
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

        // the T[N] copy function ensure_inline_array_copy already wrote, so a second copy of the
        // same array type does not build a second body. keyed on the mutable array type, which is
        // also the return type of the function
        std::unordered_map<ValueType, FunctionDeclNode *> _inline_array_copies;

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

        // every class whose payload needs tearing down gets its deinit, whether this program released
        // one or not. `__eco_release_<T>` is linkonce_odr and branches on the slot: two units that
        // disagree leak with nothing to point at
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

        // write a synthesized body at its type, not at the site that asked. a deinit / copy ctor /
        // static init is shared; the first use is the worst author. scoped so an early return cannot
        // leave the pass writing into another module
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

        // while a synthesized deinit or copy constructor is being built, minting is into *that*
        // declaration (t_open), not the function whose walk asked for it (which may already be
        // t_owned, or t_ready in the middle of its own walk). the assert is ambient, so this
        // retargets `_current_function` rather than threading a RegionState through every mint
        class BodyMutationScope
        {
        public:
            BodyMutationScope(OwnershipPass &pass, FunctionDeclNode &decl);
            ~BodyMutationScope();

            BodyMutationScope(const BodyMutationScope &) = delete;
            BodyMutationScope &operator=(const BodyMutationScope &) = delete;

        private:
            OwnershipPass &_pass;
            FunctionDeclNode *_previous = nullptr;
        };

        // --- copies ----------------------------------------------------------------------------

        // field-wise copy for a struct whose owning properties are all classes, transitively.
        // synthesized per concrete type (`Box<Handle>` can, `Box<Buffer>` cannot). not registered in
        // FunctionRegistry: a declaration minted in the fixpoint arrives after every written call was
        // already resolved
        FunctionDeclNode *ensure_copy_constructor(const ValueType &type, const TokenReference &site);

        // **the function that copies a `T[N]` whose element is not a byte copy.** T[N] has no
        // ComplexType, so there is no constructor slot to fill - CopyKind::t_elements is that fact.
        // the body is the same counted loop emit_inline_array_copies writes into a struct's copy
        // constructor for an inline-array *field*; arriving at a T[N] *value* has to produce an
        // expression, and a call is the shape every other non-byte copy already is.
        //
        // one function per interned T[N], `linkonce_odr`, so two modules that copy the same
        // `string[2]` write identical bytes and LLVM can inline the loop at a site that cares.
        // memoized on the mutable array type: a `const T[N]` source still produces a fresh T[N]
        FunctionDeclNode *ensure_inline_array_copy(const ValueType &type, const TokenReference &site);

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
