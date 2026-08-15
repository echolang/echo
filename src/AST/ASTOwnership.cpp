#include "AST/ASTOwnership.h"

#include "AST/MatchExprNode.h"


#include "AST/ConstRefExprNode.h"

#include "AST/ASTArrayLiteral.h"
#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTConstructor.h"
#include "AST/ASTControlFlow.h"
#include "AST/ASTCopy.h"
#include "AST/ASTDestruction.h"
#include "AST/ASTLastRead.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTClone.h"
#include "AST/AssignNode.h"
#include "AST/StaticPropertyExprNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/ConstIfNode.h"
#include "AST/ConstExprNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/GuardNode.h"
#include "AST/ReleaseNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/TypeCastNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/ForStatementNode.h"
#include "AST/LoopControlNode.h"
#include "AST/TemporaryBindExprNode.h"

#include <fmt/core.h>

namespace AST
{

namespace
{
    // **may the ownership pass answer this body yet?** it walks a body exactly once, ever, so every
    // "no" it records is permanent - and a body it walks too early is one whose drops were decided
    // against a tree that had not finished arriving.
    //
    // four things can still be arriving, and they are four *arms* rather than four conditions in a
    // hand-written statement switch, which is what this used to be. that switch descended into
    // statements only, so an expression could hold a transient node and answer "concrete" - which is
    // how `make()[0]` came to have its container's `&` written a round after ownership had already
    // walked past it. AST::RecursiveVisitor makes the walk total: a node kind with no visit_* does not
    // compile, and none of the four can be reached and missed
    class BodyAnswerable : public RecursiveVisitor
    {
    public:
        bool answerable = true;

        // **the flag is monotone, so the walk stops the moment it is false.** this runs once per
        // un-answered body *and* per file root on every fixpoint round, and a body that waits k rounds
        // would otherwise pay k complete traversals to re-derive the same no.
        //
        // both descent seams are pruned - the statement loop here and the expression edge below - so
        // neither a later statement nor a sibling subtree is walked once the answer is settled. nothing
        // is rewritten, so unlike the base's loop this one may hold its bound
        void visitScope(ScopeNode &node) override
        {
            for (size_t i = 0; answerable && i < node.children.size(); i++) {
                statement_edge(node.children[i].node());
            }
        }

        ExprNode *rewrite_value_edge(ExprNode *expr) override
        {
            return answerable ? RecursiveVisitor::rewrite_value_edge(expr) : expr;
        }

        // a constant reference stands for an expression nobody has substituted in yet, so nothing about
        // what a body owns can be answered while one is present.
        //
        // belt-and-braces, unlike the two arms below: AST::ConstantExpander runs *before* the fixpoint
        // rather than inside it, so a reference should never reach this walk at all. the arm is here so
        // the invariant enforces itself if that order ever moves - this walk answers a body exactly once,
        // ever, and an early yes cannot be revisited
        void visit_const_ref(ConstRefExprNode &node) override
        {
            answerable = false;
        }

        // an untyped declaration is one the monomorphizer has not re-derived yet, and a typed one
        // still mentioning a parameter is waiting on a substitution. either way there is no answer to
        // "does this own something" yet
        void visitVarDecl(VarDeclNode &node) override
        {
            if (!node.has_type() || contains_type_param(node.type())) {
                answerable = false;
            }

            RecursiveVisitor::visitVarDecl(node);
        }

        // **a match whose patterns are not decided yet is never answerable.** the bindings are untyped
        // and have no initializer until AST::MatchResolution has said which case each arm names, so a
        // walk now would resolve the arrival of a value that is about to be replaced by a payload read
        // - permanently, this pass walking a body exactly once ever, and with nothing reporting it.
        //
        // the untyped-declaration arm above catches a *bound* arm today, the bindings being scope
        // children with no type node. this one is what covers an arm that binds nothing, where there is
        // no declaration to be untyped and the arm's value would otherwise be walked against a match
        // whose own result type is still unknown
        void visit_match(MatchExprNode &node) override
        {
            if (!node.patterns_decided) {
                answerable = false;
                return;
            }

            RecursiveVisitor::visit_match(node);
        }

        // **an unexpanded array literal is never answerable**, which the declaration's type alone does
        // not say: one typed from its elements holds an *unknown* until AST::OperatorRewriter expands
        // it, and unknown is not contains_type_param. asked of the literal itself rather than of the
        // declaration holding one, so a literal in any other position counts too
        void visit_array_literal_expr(ArrayLiteralExprNode &node) override
        {
            if (!node.expansion_decided) {
                answerable = false;
            }

            RecursiveVisitor::visit_array_literal_expr(node);
        }

        // **an assignment through an undecided bracket may not be a write to a place at all.**
        // AST::OperatorRewriter::resolve_index_write replaces the whole statement with one call when the
        // container declares an element-write contract, and this pass walks a body exactly once - so a walk
        // now would decide the ownership of an assignment about to leave the tree, and push a teardown onto
        // a node nothing will emit.
        //
        // the arm below already covers it through the target, since an undecided bracket is undecided
        // wherever it sits. this one is here so the invariant is *stated where it is relied on* rather than
        // inherited: it costs nothing, and it is what keeps the rewrite sound if a future node kind ever
        // parents an AssignNode somewhere a scope's child list does not reach
        void visit_assign(AssignNode &node) override
        {
            if (node.target != nullptr
                && node.target->get_node_type() == NodeType::n_expr_index
                && !static_cast<IndexExprNode *>(node.target)->resolution_decided) {
                answerable = false;
            }

            RecursiveVisitor::visit_assign(node);
        }

        // an unresolved bracket has no element call yet, so there is nothing here for the arm below to
        // find - and resolving it is what *creates* the borrow of the container
        void visit_index_expr(IndexExprNode &node) override
        {
            if (!node.resolution_decided) {
                answerable = false;
            }

            RecursiveVisitor::visit_index_expr(node);
        }

        // **a call that has not settled has not been fitted to its parameters**, and fitting is what
        // writes the `&` around a borrow argument. so a body walked while one is outstanding gives that
        // argument's temporary no slot, and AST::TypeChecker's guard rail reports it as a compiler bug -
        // which is exactly what it did. the round order already claims this invariant in the
        // monomorphizer ("last of the four, so every call in a body it walks has already been fitted");
        // this is where the claim is checked
        //
        // a *failed* call is terminal and counts as answerable: it has its own diagnostic, and waiting
        // for a program that cannot compile only costs it its drops
        void visitFunctionCallExpr(FunctionCallExprNode &node) override
        {
            if (!call_is_terminal(node.settlement)) {
                answerable = false;
            }

            RecursiveVisitor::visitFunctionCallExpr(node);
        }

        // **an unlowered `const if` is never answerable**, and this is the arm whose absence is silent.
        // which of its two arms exists at all is not decided yet, and this pass walks a body exactly
        // once - so a walk now would resolve the ownership of statements about to be thrown away, and
        // give a `T $doomed` in the untaken arm a drop, which is one more generic call site.
        //
        // silent because walk_statement's `default:` reads `child.is_expression_node() ? ... : nullptr`,
        // and a ConstIfNode is not an expression - so without this the whole subtree would simply never
        // be walked, with no diagnostic anywhere. AST::ConstFolding runs earlier in the same round; once
        // it has, this node is gone and the arm it left behind is an ordinary scope
        void visit_const_if(ConstIfNode &node) override
        {
            answerable = false;

            RecursiveVisitor::visit_const_if(node);
        }

        // the same for its expression sibling: an unfolded `const(...)` is a value nothing knows yet, and
        // a copy or a drop decided around it would be decided against the operand's type rather than the
        // literal's - which is the same type, but only because the node is transparent *on purpose*
        void visit_const_expr(ConstExprNode &node) override
        {
            answerable = false;

            RecursiveVisitor::visit_const_expr(node);
        }

        // **an unlowered foreach is never answerable.** it declares `$el` and `$k` with no type, and
        // the iterator declaration it will mint does not exist yet. AST::ForeachLowering runs earlier
        // in the same round; once it has, this node is gone and the scope it left behind is ordinary
        void visit_foreach(ForeachNode &node) override
        {
            answerable = false;

            RecursiveVisitor::visit_foreach(node);
        }

        // **an undecided guard is never answerable**, and this arm's absence is silent in the one way
        // that matters: the binding is *typed* and its initializer resolves, so nothing here looks
        // unfinished. what is unfinished is how the binding gets filled - a question about the subject's
        // conformance that only AST::GuardLowering can answer, in the fixpoint - so a walk now would
        // resolve the arrival of a value about to be replaced by an `unwrap()` read. permanently: this
        // pass walks a body exactly once, ever.
        //
        // a `T?` guard is decided by the parser and never reaches this, so no existing program's
        // ownership answer moves by taking this arm
        void visit_guard(GuardNode &node) override
        {
            if (!node.plan_decided) {
                answerable = false;
            }

            RecursiveVisitor::visit_guard(node);
        }

        // **an unlowered interpolation is never answerable**, and this is the second arm whose absence
        // is silent. it stands for a chain of calls none of which exist yet - each one allocating a
        // `string` that owes a drop - so a walk now would decide the ownership of a statement whose
        // owning values have not been minted. AST::InterpolationLowering runs earlier in the same
        // round; once it has, what is left is ordinary calls
        void visit_string_interpolation(StringInterpolationExprNode &node) override
        {
            answerable = false;

            RecursiveVisitor::visit_string_interpolation(node);
        }

        // a nested declaration is resolved as its own body, from the file root's children. whether
        // *it* is ready says nothing about whether this one is
        void visitFunctionDecl(FunctionDeclNode &) override
        {
        }

        // **and neither does a type declaration**, which is not flow at all. its properties are
        // ordinary VarDeclNodes, so descending into `struct Box<T> { T $value; }` finds one typed `T`
        // and answers "never" for every body the struct is declared beside - a file root included
        void visit_type_decl(TypeDeclNode &) override
        {
        }
    };

    // how a diagnostic names the place a value was arriving at
    const char *describe(ValueDestination destination)
    {
        switch (destination) {
            case ValueDestination::t_declaration: return "declaration";
            case ValueDestination::t_assignment:  return "assignment";
            case ValueDestination::t_initialization: return "initialization";
            case ValueDestination::t_argument:    return "argument";
            case ValueDestination::t_return:      return "return";
        }
        return "destination";
    }

    // the local a `mv` names, or null when the operand is not a bare variable read. only a whole
    // variable can be moved out of today: moving one field (`mv $doc->body`) leaves a partly-moved
    // struct whose destructor has no defined behaviour, and moving an element (`mv $items[0]`) has a
    // runtime index with nothing static to mark unset. both are refused rather than half-supported.
    VarDeclNode *whole_variable_moved(ExprNode *operand)
    {
        if (operand == nullptr || operand->get_node_type() != NodeType::n_varref) {
            return nullptr;
        }

        return place_root_of(operand);
    }

    // the declaration a place ultimately addresses, plus the member names between the two in
    // declaration order - so `$this->a->b` answers `$this` and `["a", "b"]`.
    //
    // null when the place is not a plain member chain. an index is the case that matters: `$items[$i]`
    // names storage picked at runtime, so there is no static identity to compare two writes on, and
    // guessing one would either miss a leak or invent a diagnostic
    VarDeclNode *member_path_of(ExprNode *expr, std::vector<std::string> &path)
    {
        if (expr == nullptr) {
            return nullptr;
        }

        if (expr->get_node_type() == NodeType::n_varref) {
            return place_root_of(expr);
        }

        if (expr->get_node_type() != NodeType::n_member_access) {
            return nullptr;
        }

        auto *access = static_cast<MemberAccessNode *>(expr);
        auto &base = access->get_base_node();

        if (!base.has() || !base.is_expression_node()) {
            return nullptr;
        }

        VarDeclNode *root = member_path_of(base.unsafe_ptr<ExprNode>(), path);

        // pushed on the way *out* of the recursion, so the names come back outermost-last without a
        // reverse. a member access nests base-first, which is the opposite of how it reads
        if (root != nullptr) {
            path.push_back(access->get_member_name().value());
        }

        return root;
    }

}

OwnershipPass::OwnershipPass(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

CodeRef OwnershipPass::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, _current_file, token.make_slice()};
}

TokenReference OwnershipPass::virtual_token(const std::string &value, Token::Type type, const TokenReference &at)
{
    return _current_module->make_virtual_token(value, type, at);
}

// the module a class layout is declared in, for every declared type in the bundle. An instantiation has
// no declaration node of its own, so it answers with its template's module - which is where the
// monomorphizer already homes a function instance, and for the same reason: the tokens a clone copies
// belong to that module's collection
void OwnershipPass::build_type_module_map()
{
    _type_module.clear();

    for (auto &module_ptr : _bundle.modules) {
        for (auto *type_decl : module_ptr->nodes.of_type<TypeDeclNode>()) {
            // **the file, not just the module.** the stdlib is 21 files, so the module's first file is
            // `arr.eco` for every type in it - and a body written there takes its DWARF *file* from the
            // root that holds it while its *line* comes from the type's own token, which is a subprogram
            // no debugger can open. file_of answers null for a token another module owns or one nothing
            // spells, and then the module's first file is all there is to say
            const File *declared_in = type_decl->name_token.has_value()
                ? module_ptr->file_of(type_decl->name_token.value())
                : nullptr;

            _type_module[&type_decl->complex_type()] = TypeHome{
                module_ptr.get(),
                const_cast<File *>(declared_in),
                type_decl,
            };
        }
    }
}

void OwnershipPass::synthesize_pending_class_deinits()
{
    build_type_module_map();

    // declared classes first, then the interned instantiations. Two enumerations because a ComplexType
    // reaches the bundle by two routes and only one of them has a declaration node: `class Foo` is
    // embedded on its TypeDeclNode, `Box<int32>` is minted by TypeRegistry::get_or_create_instantiation
    // the map is unordered, so sweeping it directly would synthesize in hash order - and the order
    // declarations are appended in decides the order bodies are emitted in, which is visible in every IR
    // dump. Sorted on the mangled token, which is the one name a ComplexType has that is stable across
    // runs by construction - carried alongside rather than asked for inside the comparator, because
    // mangled_token() builds a fresh recursively-allocated string per call and this sweep runs every round
    std::vector<std::pair<std::string, ComplexType *>> candidates;

    for (const auto &[ct, home] : _type_module) {
        ComplexType *candidate = const_cast<ComplexType *>(ct);
        candidates.emplace_back(candidate->mangled_token(), candidate);
    }

    for (ComplexType *inst : _collector.type_registry.instantiations()) {
        candidates.emplace_back(inst->mangled_token(), inst);
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const auto &a, const auto &b) { return a.first < b.first; });

    for (const auto &[mangled, ct] : candidates) {
        if (!ct->is_class_kind() || ct->deinit() != nullptr) {
            continue;
        }

        // the layout guards, the home lookup and the site all belong to ensure_deinit, which every drop
        // site reaches too - what this sweep contributes is *asking* for a class nothing in this program
        // released, and the order it asks in
        ensure_deinit(ValueType::make_complex(ct), std::nullopt);
    }
}

bool OwnershipPass::run_round()
{
    _changed = false;

    // before the file walks, so the classes nothing released are asked about in the sweep's own sorted
    // order rather than interleaved with whatever the walk ran into
    synthesize_pending_class_deinits();

    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();

        for (auto &file : module_ptr->files()) {
            _current_file = &file;

            if (file.root == nullptr) {
                continue;
            }

            // the file root is a body in every sense that matters here: codegen synthesizes `main`
            // out of it, so a local declared at file scope owns its value and is destroyed at the
            // end of the program exactly as one in a function is
            resolve_root(*file.root);

            // declarations reached through the root's children, which is also where the
            // monomorphizer appends the instances it creates - so an instance body created this
            // round is resolved on the next one
            for (auto &child : file.root->children) {
                if (child.has_type<FunctionDeclNode>()) {
                    resolve_function(child.get<FunctionDeclNode>());
                }
            }
        }
    }

    // **everything this round synthesized, in one place, after every walk.** a deinit or a copy
    // constructor cannot be appended while the walk runs - resolve_function is iterating the very children
    // it would append to - and it cannot be appended per file either, because a deinit is built into the
    // file that declares its *type*, which is not the file being walked. so the whole round drains here.
    //
    // they are picked up on the next round like any other declaration, which is also when their own bodies
    // get walked: a deinit's `$this` is a borrow and owes nothing of its own, and a copy constructor's
    // field-wise assignments are exactly what has to be walked for the retains to appear. publishing set
    // `_changed`, so that round exists
    for (const PendingDecl &pending : _pending_declarations) {
        pending.file->root->add_funcdecl(*pending.decl);
    }
    _pending_declarations.clear();

    return _changed;
}

void OwnershipPass::resolve_root(ScopeNode &root)
{
    if (_processed_roots.count(&root) > 0) {
        return;
    }

    // **the same gate resolve_function has, and for the same reason.** a file root is a body too -
    // codegen synthesizes `main` out of it - and it is walked exactly once, so a "no" derived before
    // the fixpoint settled is never revisited. this used to be ungated because nothing at file scope
    // could be un-answerable on the first round; an unlowered `foreach` is, and a root walked past one
    // leaves every local the loop declares with no drop at all
    if (!body_is_concrete(root)) {
        return;
    }

    _processed_roots.insert(&root);

    _current_function = nullptr;
    _frames.clear();
    _loop_frames.clear();
    _moved.clear();
    _maybe_moved.clear();
    _initialized_storage.clear();
    _temporary_count = 0;

    // a file root has no parameters - `main` is synthesized from its statements, and takes none of
    // them from anywhere this pass can see
    _handover_reads = handover_reads_in(root, {});

    walk_scope(root);
}

void OwnershipPass::resolve_function(FunctionDeclNode &decl)
{
    // a template's body says nothing yet: whether a `T $x` local owns anything is only answerable
    // once T is bound. the monomorphizer's clone of it comes back around on a later round
    if (decl.is_generic() || decl.body == nullptr) {
        return;
    }

    if (_processed_functions.count(&decl) > 0) {
        return;
    }

    // a *non*-generic body can still be waiting on one. `$t = $box->get();` in an ordinary function
    // declares a local whose type is the template's `T` until the monomorphizer has both instantiated
    // the call and re-derived the declaration from it - and this pass runs in the same round loop, so
    // it can arrive first. asking needs_destruction then answers "no" for what is really a class, and
    // because a body is processed exactly once that answer would never be revisited: the local would
    // silently never be released
    if (!body_is_concrete(*decl.body)) {
        return;
    }

    _processed_functions.insert(&decl);

    _current_function = &decl;
    _frames.clear();
    _loop_frames.clear();
    _moved.clear();
    _maybe_moved.clear();
    _initialized_storage.clear();
    _temporary_count = 0;

    // a by-value parameter of an owning type owns what it was handed - "$items is ours; it is
    // destroyed at the end of this body". seeded into the *body's* frame rather than a frame of its
    // own, and ahead of the body's locals, so reverse-order drops destroy the body's locals first
    // and the parameters last.
    //
    // an *implicit* parameter is skipped by position, because it is never the callee's to destroy: a
    // method's receiver is a borrow of storage the caller owns, and a closure's environment belongs to
    // the callable value that was called, which may well outlive this call. the receiver needed no
    // special case - a borrow is a pointer, so needs_destruction already answered no - but a closure's
    // environment is a class *handle*, and by-value class parameters are exactly the ones this owns
    std::vector<VarDeclNode *> owned_params;
    std::vector<VarDeclNode *> declared_params;

    for (size_t i = decl.implicit_arg_count(); i < decl.args.size(); i++) {
        VarDeclNode *arg = decl.args[i];

        if (arg == nullptr) {
            continue;
        }

        declared_params.push_back(arg);

        if (arg->has_type() && needs_destruction(arg->type())) {
            owned_params.push_back(arg);
        }
    }

    // **the explicit parameters only**, for the reason the frame above skips the implicit ones: a
    // receiver is a borrow and a closure's environment belongs to the callable that was called, so
    // neither is this body's to hand over however dead it is afterwards
    _handover_reads = handover_reads_in(*decl.body, declared_params);

    _frames.push_back(Frame{decl.body, owned_params});
    walk_scope(*decl.body);
    _frames.pop_back();
}

bool OwnershipPass::body_is_concrete(ScopeNode &scope) const
{
    BodyAnswerable answerable;
    scope.accept(answerable);

    return answerable.answerable;
}

ExitKind OwnershipPass::walk_scope(ScopeNode &scope)
{
    // the function body's frame is pushed by resolve_function, which seeds it with the parameters
    // every other scope opens its own
    const bool own_frame = _frames.empty() || _frames.back().scope != &scope;

    if (own_frame) {
        _frames.push_back(Frame{&scope, {}});
    }

    // rebuilt rather than mutated in place: a `return` needs its drops *before* it, and inserting
    // into the vector being iterated is how an off-by-one turns into a dropped statement
    NodeReferenceList rebuilt;
    rebuilt.reserve(scope.children.size());

    for (auto &child : scope.children) {
        // what walk_statement hands back is what the scope keeps - normally the statement itself, but a
        // discarded owning temporary is replaced by the declaration that now owns it
        const NodeReference kept = walk_statement(child);

        // every value edge either binds a requested temporary or refuses it, and a statement is made
        // of value edges - so nothing may still be pending here. the day a statement kind is added
        // that does not flush, this says so instead of silently dropping the temporary's teardown
        assert(_pending_temporaries.empty() && "a temporary outlived the statement that asked for it");

        if (child.has_type<ReturnNode>()) {
            // a return leaves every enclosing scope at once, so it owes the drops of all of them,
            // innermost frame first. one of the three insertion points in the language: this, the
            // `break`/`continue` arm below, and the end of a scope
            //
            // collected onto the return, not ahead of it: the returned expression may read what is being
            // dropped, and codegen evaluates the expression before running these - see ReturnNode::unwind
            // the floor is 0: a return leaves the function, so no frame outlives it
            collect_unwind(0, child.get_ptr<ReturnNode>()->unwind);
        }
        else if (child.has_type<LoopControlNode>()) {
            // **the same edge, with a floor.** a `break` leaves every frame from here down to the loop
            // body's, inclusive - and no further. the frames outside the loop are still live on the
            // other side of the branch, which is the whole of what makes this shorter than a return's
            //
            // Parser::parse_loop_control refuses one outside a loop and builds no node for it, so an
            // empty stack here is a compiler bug rather than a program error
            assert(!_loop_frames.empty() && "a loop exit reached the ownership pass with no enclosing loop");

            auto *loop_exit = child.get_ptr<LoopControlNode>();

            collect_unwind(_loop_frames.back().frame_floor, loop_exit->unwind);

            // **the moved state travels with the branch.** where it goes is the loop's exit rather than
            // the join after whatever `if` this sits in, so it is recorded on the loop frame and merged
            // by the loop arm. the same edge as the unwind above and for the same reason: this is the
            // point where what the branch carries out of here is known
            auto &carried = loop_exit->kind == LoopControlKind::t_break
                ? _loop_frames.back().break_moved
                : _loop_frames.back().continue_moved;

            carried.insert(_moved.begin(), _moved.end());
        }

        rebuilt.push_back(kept);
    }

    // spliced *before* the question below rather than after. the question reads the scope's statements,
    // and walk_statement is allowed to replace one - a discarded owning temporary comes back as the
    // declaration that now owns it - so asking it of the pre-walk list would answer about a tree that no
    // longer exists
    scope.children = std::move(rebuilt);

    // the scope's own locals, destroyed in reverse declaration order, at the point after its last
    // statement. Skipped when control never reaches that point: whatever left already owes every
    // frame's drops and collected them onto its own ReturnNode::unwind, so a second set here is dead
    // tree.
    //
    // Dead, and not free. It is type-checked, and a drop of a `Box<int32>` local *creates a generic
    // call site* the monomorphizer then instantiates. It also shows up in `-ar`, which is precisely
    // where a duplicated drop is supposed to be diagnosed rather than printed.
    //
    // Asked of AST::scope_always_exits rather than re-derived here, and that is the fix. This used to
    // test the *last child* for `ReturnNode` and nothing else, so a `die` tail, an `if` whose arms both
    // return, and any statement written after a `return` each appended a full duplicate. None of them
    // ever reached codegen, because gen_scope stops at the first terminated block - so the tree was
    // wrong and the binary was not, which is the kind of divergence `-ar` exists to make visible.
    //
    // Computed once and handed back. The arms above this one want the same answer about the scope they
    // asked for, and asking it a second time is a second walk of the whole subtree
    const ExitKind exit = scope_exit_kind(scope);

    if (exit == ExitKind::t_none) {
        collect_frame_drops(_frames.back(), scope.children);
    }

    if (own_frame) {
        // **a local leaves the moved-from set with its scope.** nothing outside this block can read it
        // or owes it a drop, so carrying it out is carrying an answer about a variable that no longer
        // exists - and the `if` merge one level up reads exactly that set. a value declared *and*
        // handed over inside one arm was being reported as "moved out of on only one branch", against
        // an other branch in which the variable was never declared at all
        for (const VarDeclNode *local : _frames.back().locals) {
            _moved.erase(local);
            _maybe_moved.erase(local);
        }

        _frames.pop_back();
    }

    return exit;
}

NodeReference OwnershipPass::walk_statement(const NodeReference &child)
{
    Node *node = child.node();

    if (node == nullptr) {
        return child;
    }

    switch (node->get_node_type()) {
        case NodeType::n_vardecl:
        {
            auto *decl = static_cast<VarDeclNode *>(node);

            const ValueType type = decl->has_type() ? decl->type() : ValueType::make_unknown();

            decl->init_expr = resolve_value_arrival(decl->init_expr, type, nullptr, ValueDestination::t_declaration);

            // tracked *after* the initializer is resolved, so `Buffer $b = mv $b;` cannot mark the
            // variable it is still declaring
            if (needs_destruction(type)) {
                _frames.back().locals.push_back(decl);
            }
            break;
        }

        case NodeType::n_assign:
        {
            auto *assign = static_cast<AssignNode *>(node);

            // **the target refuses.** writing into a member or an element of a value with no storage of
            // its own: the bytes are destroyed at the end of this statement, so nothing will ever read
            // what was written. refused rather than bound, which is also what keeps AssignNode::target a
            // place - the wrapper would not be one
            MaterializationScope target_scope(*this,
                "writing to", "would be lost, because the value is destroyed at the end of this statement");

            // a whole-variable target is *written*, not read, so a moved-from variable being re-seated
            // is not a use-after-move. the arm below already says so - it clears the moved state and
            // notes "the variable is live again from here on" - but the walk got there first and
            // reported the write as a read. any other target shape (`$a->f`, `$a[$i]`) genuinely does
            // read `$a` to find the storage, so it is walked as usual
            if (whole_variable_moved(assign->target) == nullptr) {
                assign->target = walk_expression(assign->target);
            }

            target_scope.close(assign->target);

            const ValueType target_type =
                assign->target != nullptr ? value_result_type(*assign->target) : ValueType::make_unknown();

            // keyed on hands_over_value rather than on is_initialization, which it used to be. both are
            // set by the synthesized field-wise constructor, but a *hand-written* constructor writes
            // fresh storage too and its transfers stay visible - it says `$this->data = mv $data`. so
            // its `$this->inner = $other->inner` is an ordinary assignment, and reaches the copy
            // constructor rather than silently moving out of a borrowed source
            assign->value_expr = resolve_value_arrival(
                assign->value_expr,
                target_type,
                nullptr,
                assign->hands_over_value
                    ? ValueDestination::t_initialization
                    : ValueDestination::t_assignment);

            // **a class target releases whatever it held, and codegen orders the sequence.**
            //
            // It cannot be a node with a place of its own the way a struct's teardown is. The release
            // needs the old handle out of the slot codegen has already addressed, and a class target
            // may be `$node->next` or an element, whose index expression must not be evaluated twice.
            // So the flag says *that* the old reference is owed a release, and gen_assign says *when* -
            // retain the new value, read the old handle, store, release the old.
            //
            // It also lifts the whole-variable restriction below. Writing an owning *struct* into a
            // field is the unspecified partial-ownership case, but a class field holds one handle and
            // replacing it is completely defined. That is what makes `$node->next = $other` - and so
            // any linked structure at all - expressible
            // "a copy of this is one more reference" is exactly what AST::classify_copy answers with
            // t_retain, and it is already the compiler's one classifier - asked rather than respelled,
            // so the next reference-counted kind does not have to find this site
            if (!assign->is_initialization && classify_copy(target_type) == CopyKind::t_retain) {
                VarDeclNode *root =
                    assign->target != nullptr ? whole_variable_moved(assign->target) : nullptr;

                // a moved-from variable owes nothing: the reference travelled with the value, and the
                // handle still sitting in the slot is somebody else's. releasing it here would be the
                // second release of one reference. a field or an element target (no root) always owes
                // one - a field cannot be moved out of, so it always still holds what it was given
                assign->releases_old = root == nullptr || _moved.count(root) == 0;

                // a callable's teardown is uniform and needs no per-type deinit - see emit_drop
                if (assign->releases_old && target_type.is_class()) {
                    // the release codegen emits here calls the same thunk a scope-exit release does,
                    // so the deinit has to exist by the time it is reached
                    ensure_deinit(target_type, location_of_expression(assign->target));
                }

                if (root != nullptr) {
                    // the assignment re-seats it: it holds a value again, so it is readable again and
                    // owes a release again at the end of its scope
                    _moved.erase(root);
                    _maybe_moved.erase(root);
                }
                break;
            }

            // an initialization owes no teardown - the storage is fresh - but that is only true of the
            // *first* one. a constructor writing the same owning field twice would leak whatever the
            // first write built, and nothing further down would notice, because both writes claim there
            // was nothing there. so the claim is checked rather than trusted
            if (assign->is_initialization && needs_destruction(target_type)) {
                std::vector<std::string> path;

                if (VarDeclNode *root = member_path_of(assign->target, path)) {
                    // the declaration plus the names between it and the place is exactly what makes two
                    // writes the same write. the pointer is in the key because two constructors of two
                    // structs both spell their first property `$this->x`
                    std::string key = fmt::format("{}", static_cast<const void *>(root));
                    std::string description = root->name_full();

                    for (const auto &segment : path) {
                        key += "->" + segment;
                        description += "->" + segment;
                    }

                    if (!_initialized_storage.insert(key).second) {
                        _collector.collect_issue<Issue::GenericError>(
                            code_ref_for(assign->token_assign), fmt::format(
                                "'{}' is initialized twice, and '{}' owns a resource - the value the first "
                                "write built would never be destroyed. Build it once, or assign the whole "
                                "variable instead so the old value is torn down.",
                                description,
                                target_type.get_type_desciption()
                            )
                        );
                    }
                }
            }

            // "the old value is destroyed, the new one is built in place". only for a whole
            // variable: writing an owning value into a *field* is the partial-ownership case, whose
            // drop the enclosing struct's destructor would have to know about, and that is one of
            // the chapter's unspecified holes. reported rather than silently leaked
            if (!assign->is_initialization && needs_destruction(target_type)) {
                VarDeclNode *root =
                    assign->target != nullptr ? whole_variable_moved(assign->target) : nullptr;

                if (root == nullptr) {
                    _collector.collect_issue<Issue::GenericError>(
                        code_ref_for(assign->token_assign), fmt::format(
                            "Cannot assign a '{}' into a field or element - it owns a resource, and "
                            "replacing part of a value is not supported yet. Assign the whole variable, "
                            "or release the old value first.",
                            target_type.get_type_desciption()
                        )
                    );
                }
                else {
                    // whatever the variable held is being replaced, so it is destroyed first -
                    // unless it holds nothing, having been moved out of already
                    //
                    // carried *on* the assignment rather than pushed ahead of it: gen_assign runs
                    // these after the right-hand side and before the store, which is the only window
                    // in which both the old value and the new one exist. see AssignNode::teardown_old
                    if (_moved.count(root) == 0) {
                        auto &teardown = _current_module->nodes.emplace_back<ScopeNode>();

                        std::vector<std::string> path;
                        emit_drop(root, path, target_type, teardown.children);

                        if (!teardown.children.empty()) {
                            assign->teardown_old = &teardown;
                        }
                    }

                    // the variable is live again from here on
                    _moved.erase(root);
                    _maybe_moved.erase(root);
                }
            }
            break;
        }

        case NodeType::n_func_return:
        {
            auto *ret = static_cast<ReturnNode *>(node);

            const ValueType wanted = _current_function != nullptr
                ? _current_function->get_return_type()
                : ValueType::make_unknown();

            // the implicit move of a returned local lives in resolve_value_arrival, next to the
            // explicit one, so the two cannot disagree about what "moved" means
            ret->expr = resolve_value_arrival(ret->expr, wanted, nullptr, ValueDestination::t_return);
            break;
        }

        // a `break` carries no expression, so there is nothing to walk. its unwind list is filled by
        // walk_scope, which is where _frames is - named here rather than left to `default:`, because that
        // arm calls walk_value_edge on a node that is not an expression
        case NodeType::n_loop_control:
            break;

        case NodeType::n_guard:
        {
            auto *stmt = static_cast<GuardNode *>(node);
            VarDeclNode *decl = stmt->decl;

            // the *declared* type - the non-null one - because that is what the binding holds. derived
            // once: the arrival below and the frame push at the end of this arm are the same question
            const ValueType type = decl != nullptr && decl->has_type()
                ? decl->type()
                : ValueType::make_unknown();

            // **the binding is an ordinary value arrival**, and that is the whole reason `guard` needed no
            // ownership rule of its own: `guard Res $r = $maybe` is a copy of a place, so it retains, and
            // `$r` joins the frame's locals and is released at the scope's end like any other
            if (decl != nullptr) {
                // **a tagged optional's payload is copied out of the pair, not out of the pair's own
                // address.** the initializer stays exactly as written, because it is also the value
                // codegen *tests* - so the copy hangs off `bound_value` instead, over the `__value` place.
                //
                // only for a **place**: a call result is a wrapper nobody owns, so moving the payload out
                // of it is right and is what codegen has always done. asking arrival at the payload here
                // is also what makes an owning payload with no copy at all a located error at the guard,
                // in the same words `$b = $a` gets
                ExprNode *tested = decl->init_expr;

                // **and the arm is keyed on which edge feeds the binding, not on what the initializer
                // looks like.** a null `presence_test` is the `T?` form - the one where `init_expr` is
                // both the value tested and the thing unwrapped, which is what makes the payload read
                // below necessary. with one set, AST::GuardLowering has already put the protocol's
                // `deref(unwrap())` on `init_expr` and there is nothing to reach inside: the else arm is
                // an ordinary declaration arrival and is the whole of what a protocol guard owes.
                //
                // reachable rather than tidy: a payload that is *itself* a tagged optional makes
                // `deref(unwrap())` answer is_wrapped_optional() **and** is_place_expression(), so
                // without this the protocol path would read `__value` out of the payload it was handed
                if (stmt->presence_test == nullptr && tested != nullptr
                    && tested->result_type().is_wrapped_optional() && is_place_expression(*tested)) {
                    stmt->bound_value = resolve_value_arrival(
                        optional_payload_place(tested, stmt->token),
                        type,
                        nullptr,
                        ValueDestination::t_declaration);
                }
                else {
                    decl->init_expr = resolve_value_arrival(
                        decl->init_expr, type, nullptr, ValueDestination::t_declaration);
                }
            }

            // the else arm is walked with the moved-set *saved and restored*, exactly as an `if` arm is:
            // it is a branch, and what it moves out of does not reach the code after the guard
            //
            // no merge, though, and that is the difference from an `if`: the arm cannot fall through
            // (Parser::parse_guard refused one that could), so there is no path on which its moves are
            // visible afterwards. taking the union would mark things moved that this statement's
            // continuation can still legitimately read
            //
            // where they *are* visible is wherever the arm went, and nothing here has to arrange that: an
            // arm leaving by `break` recorded them on the enclosing loop's frame as it was walked
            if (stmt->else_scope != nullptr) {
                const auto before = _moved;
                const auto maybe_before = _maybe_moved;

                walk_scope(*stmt->else_scope);

                _moved = before;
                _maybe_moved = maybe_before;
            }

            // **the binding joins the frame after the else arm, not before it.** Parser::parse_guard
            // refuses an else arm that can fall through, so the binding is unreachable there - and while it
            // was pushed first, a `return` inside that arm collected a drop of it. For a class that was a
            // release of a null slot and survivable; for a payload with a written destructor it ran one,
            // on the path that never bound anything
            if (decl != nullptr && needs_destruction(type)) {
                _frames.back().locals.push_back(decl);
            }

            break;
        }

        case NodeType::n_if_statement:
        {
            auto *stmt = static_cast<IfStatementNode *>(node);
            stmt->condition = walk_value_edge(stmt->condition);

            // each arm moves out of its own copy of the state, and the arms that *reach the code after
            // the `if`* are merged by union: a variable moved on either side is unset afterwards.
            // reading pessimism into that is the wrong way round - the alternative is a variable whose
            // validity you can only determine by simulating the branch in your head
            const auto before = _moved;
            const auto maybe_before = _maybe_moved;

            // **one arm's contribution to what follows the `if`**, so the two arms are walked by one
            // piece of code rather than by two that have to be edited symmetrically
            //
            // an arm with no block falls through, moving nothing - which is what the snapshot it starts
            // out as says. an arm that *has* a block hands its result over rather than being copied out
            // of: the walk of the next arm re-seats `_moved` from the snapshot anyway, so the arm's set
            // has no second reader
            struct ArmState
            {
                bool joins = true;
                std::unordered_set<const VarDeclNode *> moved;
                std::unordered_set<const VarDeclNode *> maybe_moved;
            };

            const auto walk_arm = [&](ScopeNode *arm) -> ArmState {
                if (arm == nullptr) {
                    return ArmState { true, before, maybe_before };
                }

                _moved = before;
                _maybe_moved = maybe_before;

                ArmState state;
                state.joins = walk_scope(*arm) == ExitKind::t_none;
                state.moved = std::move(_moved);
                state.maybe_moved = std::move(_maybe_moved);

                return state;
            };

            auto if_arm = walk_arm(stmt->if_scope);
            auto else_arm = walk_arm(stmt->else_scope);

            _moved = before;
            _maybe_moved = maybe_before;

            // **neither arm comes back.** the code after the `if` is unreachable, so there is no state
            // for it to be wrong about. what each arm moved has already gone where it belongs - onto a
            // `return`'s unwind, or onto the enclosing loop's frame
            if (!if_arm.joins && !else_arm.joins) {
                break;
            }

            // **an arm that leaves contributes nothing to the join.** it does not reach the code after
            // the `if`, so what it moved is not visible there - and it is not an "other branch" for the
            // arm that does reach it to disagree with. a constructor whose `if` arm returns `$this`
            // moves it on that path only, and merging that into the fall-through is what used to read
            // as a conditional move
            //
            // spelled as the remaining arm standing in for the one that left: it is then both sides of
            // the comparison below, so the union is its own state and there is nothing to report
            if (!if_arm.joins) {
                if_arm.moved = else_arm.moved;
                if_arm.maybe_moved = else_arm.maybe_moved;
            }
            else if (!else_arm.joins) {
                else_arm.moved = if_arm.moved;
                else_arm.maybe_moved = if_arm.maybe_moved;
            }

            // a decl stays *definitely* moved only where no reaching arm was unsure about it, which is
            // why this is the arms' own sets rather than the snapshot: an arm that moved outright what
            // was merely maybe-moved before the `if` erased it, and that erase must survive the merge
            _maybe_moved = if_arm.maybe_moved;
            _maybe_moved.insert(else_arm.maybe_moved.begin(), else_arm.maybe_moved.end());

            // moved on one side and not the other is a conditional move, whichever side that is - so the
            // union runs twice over the same body rather than being written out per side
            const auto join = [&](const auto &arm, const auto &other) {
                for (const auto *decl : arm) {
                    if (_moved.insert(decl).second && other.count(decl) == 0) {
                        _maybe_moved.insert(decl);
                        report_conditional_move(decl);
                    }
                }
            };

            join(if_arm.moved, else_arm.moved);
            join(else_arm.moved, if_arm.moved);
            break;
        }

        case NodeType::n_while_statement:
        {
            auto *stmt = static_cast<WhileStatementNode *>(node);

            // no step: a `while`'s condition *is* its step, and so is the advance of the `foreach` that
            // lowers into one
            walk_loop(stmt->condition, stmt->loop_scope, nullptr);
            break;
        }

        case NodeType::n_for_statement:
        {
            auto *stmt = static_cast<ForStatementNode *>(node);

            walk_loop(stmt->condition, stmt->loop_scope, stmt->step);
            break;
        }

        case NodeType::n_scope:
        {
            walk_scope(*static_cast<ScopeNode *>(node));
            break;
        }

        case NodeType::n_func_decl:
            // resolved as its own body, from the file root's children. a nested declaration is not
            // part of the enclosing scope's flow
            break;

        default:
        {
            ExprNode *original = child.is_expression_node() ? child.unsafe_ptr<ExprNode>() : nullptr;
            ExprNode *expr = walk_value_edge(original);

            // a statement that *is* an expression discards whatever it evaluated to. when that value
            // owns something - `Buffer(...);` or `Res(...);` written for its side effects - nothing
            // would ever destroy it
            //
            // bound to a synthesized local rather than reported, because there is a correct answer and
            // it costs one node: the frame now owns it and drops it at the end of the scope, through
            // exactly the machinery a named local uses. a *place* is somebody else's already, which is
            // what the second test excludes
            if (expr != nullptr && !is_place_expression(*expr) && needs_destruction(expr->result_type())) {
                return make_ref(bind_discarded_temporary(expr));
            }

            // the flush above may have wrapped the statement in the node that owns its temporary, and
            // this is the statement's own edge - the one position where the replacement is a
            // NodeReference rather than a field
            if (expr != nullptr && expr != original) {
                return NodeReference(expr->get_node_type(), expr);
            }

            break;
        }
    }

    return child;
}

// **the one loop walk**, which both loop statements above go through. what a `for` adds is the step, and
// it is walked *inside* the loop's frame and after the body: it runs on the fall-through and on every
// `continue`, so a temporary it materializes lives and dies there, once per iteration
void OwnershipPass::walk_loop(ExprNode *&condition, ScopeNode *body, ScopeNode *step)
{
    // a value edge, and the one that most needs to be: a temporary bound here is bound and
    // destroyed *inside* the block the condition is evaluated in, once per iteration. hoisting
    // it to the statement would acquire it every turn and release it once
    condition = walk_value_edge(condition);

    // a move inside a loop body runs on every iteration, so a variable declared *outside* the
    // loop would be moved out of twice - the second iteration reading a value that is no
    // longer there. the locals the loop declares itself are fine: each iteration gets its own
    if (body != nullptr) {
        std::unordered_set<const VarDeclNode *> outer;
        for (const auto &frame : _frames) {
            outer.insert(frame.locals.begin(), frame.locals.end());
        }

        const auto before = _moved;

        // the frame walk_scope is about to push for the body, recorded here rather than inside
        // it: walk_scope does not know whose scope it has been handed, and its `own_frame` test
        // only tells it whether the frame it needs is already there
        _loop_frames.push_back(LoopFrame{_frames.size(), {}, {}});
        const ExitKind body_exit = walk_scope(*body);

        // the step, still inside the loop frame - it is part of an iteration, not of what comes
        // after one. a `for` whose body always leaves reaches it only through a `continue`, and
        // through nothing at all when there is none; it is walked either way, because codegen
        // emits the block either way and an unwalked scope is a block with no drops in it
        if (step != nullptr) {
            walk_scope(*step);
        }

        const LoopFrame frame = std::move(_loop_frames.back());
        _loop_frames.pop_back();

        // **the state that reaches the back edge**, which is what the diagnostic below is about:
        // the body's fall-through where it has one, plus every `continue`. a body that always
        // leaves has no fall-through, so the header is re-entered only by a `continue` - and by
        // nothing at all if there is none
        auto back_edge = body_exit == ExitKind::t_none ? _moved : before;
        back_edge.insert(frame.continue_moved.begin(), frame.continue_moved.end());

        // **and the state that reaches the code after the loop**: the back edge - the condition
        // is what the loop is left by, and it is read from the header - plus every `break`
        auto after_loop = back_edge;
        after_loop.insert(frame.break_moved.begin(), frame.break_moved.end());

        // judged over both, and that is the over-approximation this keeps. a `break` runs at
        // most once, so moving an outer local on that path does not repeat - but the loop can
        // also be left through its condition, where the value was never moved and the drop after
        // the loop would run on it, and AST::scope_exit_kind deliberately answers nothing about
        // a loop's trip count (`while (true)` included). so "moved anywhere inside the loop"
        // stays the rule, and a break-only move is refused with the rest
        for (const auto *decl : after_loop) {
            if (before.count(decl) > 0 || outer.count(decl) == 0) {
                continue;
            }

            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(decl->token_varname), fmt::format(
                    "'{}' is moved out of inside a loop, so the next iteration would move a "
                    "value that is no longer there. Move it after the loop, or declare it "
                    "inside one.",
                    decl->name_full()
                )
            );
        }

        // a decl only a `break` moved is one the condition-exit path did not, so a read after
        // the loop says *may* have been moved rather than claiming it was
        for (const auto *decl : frame.break_moved) {
            if (back_edge.count(decl) == 0) {
                _maybe_moved.insert(decl);
            }
        }

        _moved = std::move(after_loop);
    }
}

VarDeclNode &OwnershipPass::make_temporary(ExprNode *init, const TokenReference &site)
{
    // **numbered, because one bind can hold several.** two temporaries in one statement are two
    // declarations either way - a place refers to the VarDeclNode and never to its spelling - but a
    // dump in which both read `$__temp` cannot be asserted about, and a `RAST` golden is how every
    // rule in this pass is pinned. per body rather than global, so a case's golden does not move
    // when an unrelated function above it grows a temporary
    const TokenReference name_token =
        virtual_token(fmt::format("$__temp{}", ++_temporary_count), Token::Type::t_varname, site);

    auto &type_node = _current_module->nodes.emplace_back<TypeNode>(init->result_type());
    auto &decl = _current_module->nodes.emplace_back<VarDeclNode>(name_token, &type_node);

    // no retain: the value is a non-place, so it is already the one reference nobody else holds -
    // the same rule that lets `Foo $a = Foo();` bind without one
    decl.init_expr = init;

    _changed = true;

    return decl;
}

VarDeclNode &OwnershipPass::bind_discarded_temporary(ExprNode *expr)
{
    VarDeclNode &decl = make_temporary(expr, location_of_expression(expr));

    // owned by the frame, so the scope's ordinary reverse-order drop covers it with no special case.
    // that is the whole difference from the expression-scoped temporary bind_pending_temporaries
    // builds: a statement's value lives as long as the scope, and there is nothing shorter to hang it on
    _frames.back().locals.push_back(&decl);

    return decl;
}

ExprNode *PendingEdge::get() const
{
    return slot != nullptr ? *slot : base->unsafe_ptr<ExprNode>();
}

void PendingEdge::set(ExprNode *place) const
{
    if (slot != nullptr) {
        *slot = place;
        return;
    }

    // a NodeReference rather than a field, so the tag travels with the pointer
    *base = NodeReference(place->get_node_type(), place);
}

PendingEdge OwnershipPass::pending_edge(ExprNode *owner) const
{
    switch (owner->get_node_type()) {
        case NodeType::n_expr_addrof:
            return {&static_cast<AddrOfExprNode *>(owner)->operand, nullptr};

        // the form that reads *through* a nullable holds its base in a named field. it is here because a
        // `weak<T>` operand was upgraded on the way in, and an upgrade **retains** - so the handle it is
        // branching on is one reference nobody holds, and without a temporary to own it the object it
        // names is never released. `guard` needs no arm: its binding is a declared local, which already
        // owns one, and `??` registers nothing at all (see the walker's arm for why)
        case NodeType::n_expr_optional_chain:
            return {&static_cast<OptionalChainExprNode *>(owner)->base, nullptr};

        case NodeType::n_member_access:
            return {nullptr, &static_cast<MemberAccessNode *>(owner)->get_base_node()};

        // **the one call that registers, and it registers its argument.** unlike the three above, the
        // owner is not asking for an address - it is asking for something to *own* the value it printed,
        // which is the same slot and the same drop for a different reason. an ordinary call never
        // registers, so the same AST::is_print_call the walker gates on is asked here too: a second call
        // shape that starts registering one hits the assert below rather than silently claiming argument 0
        case NodeType::n_expr_call:
        {
            auto *call = static_cast<FunctionCallExprNode *>(owner);

            if (is_print_call(*call) && !call->arguments.empty()) {
                return {&call->arguments[0], nullptr};
            }

            break;
        }

        default:
            break;
    }

    // the arms above are exactly the sites that push onto _pending_temporaries. one more reaching here
    // means a request was added without an edge, which would otherwise blind-cast
    assert(false && "a pending temporary was requested by a node with no operand edge");
    return {};
}

OwnershipPass::MaterializationScope::MaterializationScope(OwnershipPass &pass) :
    _pass(pass), _mark(pass._pending_temporaries.size())
{
}

OwnershipPass::MaterializationScope::MaterializationScope(
    OwnershipPass &pass,
    const char *action,
    const char *outcome
) :
    _pass(pass), _mark(pass._pending_temporaries.size()), _action(action), _outcome(outcome)
{
    assert(action != nullptr && outcome != nullptr && "a refusal has to say why");
}

ExprNode *OwnershipPass::MaterializationScope::close(ExprNode *value)
{
    assert(!_closed && "a materialization scope closed twice");
    _closed = true;

    // **a refusal answers the value unchanged.** it reports and leaves the tree exactly as it was
    // written, which is what lets the reader see the program they wrote rather than the rewrite the
    // compiler was part way through
    if (_action != nullptr) {
        _pass.refuse_pending_temporaries(_mark, _action, _outcome);
        return value;
    }

    return _pass.bind_pending_temporaries(value, _mark);
}

OwnershipPass::MaterializationScope::~MaterializationScope()
{
    // **a scope that was never closed left its requests on the queue**, where the next position to
    // close would adopt them - binding a temporary for an operand it knows nothing about, or refusing
    // one with somebody else's wording. neither shows up as a crash, so it is asserted rather than
    // discovered
    assert(_closed && "a materialization scope was opened and never closed");
}

void OwnershipPass::request_storage_for(ExprNode *owner)
{
    _pending_temporaries.push_back(owner);
}

std::string OwnershipPass::describe_pending(ExprNode *owner) const
{
    // tested on the one owner that *has* a member to name, rather than on the one that has not: the
    // other way round every kind added later falls into the member arm and is cast to something it is not
    if (owner->get_node_type() == NodeType::n_member_access) {
        return fmt::format(
            "its member '{}'", static_cast<MemberAccessNode *>(owner)->get_member_name().value());
    }

    return "it";
}

ExprNode *OwnershipPass::bind_pending_temporaries(ExprNode *value, size_t mark)
{
    if (_pending_temporaries.size() <= mark || value == nullptr) {
        return value;
    }

    // **a pointer read out of a temporary is an address into it**, which is the same refusal `&` earns
    // one line further out and the last spelling that could still hand one out. it is also the only
    // dangling shape AST::TypeChecker's "cannot return the address of a local" cannot see, because the
    // wrapper is not a place and AST::place_root_of finds no variable under it
    //
    // and it is what keeps AST::PointerAdjuster's arm a plain as_value: a pointer-typed body would
    // otherwise collect the deref a value position means, handing back the pointee where the
    // destination asked for the pointer
    if (value->result_type().is_pointer()) {
        refuse_pending_temporaries(mark,
            "the pointer in", "would be an address into a value destroyed at the end of this statement");
        return value;
    }

    // **nothing is inserted around the body here, and that is the whole of why the flush is last.**
    //
    // an owning value read out of the temporary needs a copy - a class-typed member is one more
    // reference, and the teardown below is about to take one away. every position that *keeps* the
    // value is an arrival, and arrive_value has already applied the copy rule to it while it was still
    // the place it was written as: a RetainExprNode for a class, a copy constructor call for a struct
    // that declares one, and the copy diagnostic for one that cannot. so the body arriving here is
    // already one reference nobody else holds
    //
    // every *other* value edge - a comparison operand, an index, a cast, a condition - reads the value
    // and drops it, and inserting a retain for those is not a safety net but a leak: nothing downstream
    // owes the matching release. that was tried, and `$b->make()->node == null` never ran the
    // destructor at all
    auto &bind = _current_module->nodes.emplace_back<TemporaryBindExprNode>(
        value, location_of_expression(_pending_temporaries[mark]));

    // recording order *is* binding order: the walk reaches an inner request first, and an inner
    // temporary holds the storage the outer one reads through, so it has to exist first
    for (size_t i = mark; i < _pending_temporaries.size(); i++) {
        ExprNode *owner = _pending_temporaries[i];

        const PendingEdge edge = pending_edge(owner);

        VarDeclNode &temp = make_temporary(edge.get(), location_of_expression(owner));
        bind.temporaries.push_back(&temp);

        // **and the operand becomes a place**, which is the whole of what codegen needed: it is a
        // varref now, so gen_member_lvalue addresses it through the arm a named local uses and `&`
        // takes the address of an alloca. no codegen arm had to learn that temporaries exist
        edge.set(make_place(&temp, {}));
    }

    // reverse binding order, as a scope's end uses and for the same reason: the last thing built is
    // the first thing torn down. through emit_drop, so what a temporary owes and what a local owes are
    // one decision - including the deinit a class release needs, which emit_drop ensures
    //
    // gated on needs_destruction exactly as the two arms that track a *local* are, and for the reason
    // that gate exists: emit_drop's arms answer the shapes that owe a release and then reach for the
    // ComplexType, so a type that has none has to be answered before it is asked. a
    // temporary can be a primitive - `inc(41)` binds an int32 - so this is the first caller that can
    // hand it one. the teardown simply stays empty, which is the common case and the correct one
    for (size_t i = bind.temporaries.size(); i-- > 0;) {
        VarDeclNode *temp = bind.temporaries[i];

        if (!needs_destruction(temp->type())) {
            continue;
        }

        std::vector<std::string> path;
        emit_drop(temp, path, temp->type(), bind.teardown);
    }

    _pending_temporaries.resize(mark);
    _changed = true;

    return &bind;
}

void OwnershipPass::refuse_pending_temporaries(size_t mark, const char *action, const char *outcome)
{
    for (size_t i = mark; i < _pending_temporaries.size(); i++) {
        ExprNode *owner = _pending_temporaries[i];

        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(location_of_expression(owner)), fmt::format(
                "'{}' has no storage of its own, so {} {} {}. Bind it to a variable first.",
                pending_edge(owner).get()->result_type().get_type_desciption(),
                action,
                describe_pending(owner),
                outcome
            )
        );
    }

    // discarded rather than bound: nothing has been rewritten yet, so the tree stays exactly as it was
    // written and the reader sees the program they wrote
    _pending_temporaries.resize(mark);
}

void OwnershipPass::report_conditional_move(const VarDeclNode *decl)
{
    // the chapter's rule - "an `if` that moves on one side and not the other leaves the variable
    // unset afterwards" - settles *reading* the variable, and the merge above implements it. what it
    // does not answer is who destroys the value on the branch that did *not* move it: the variable is
    // unset, so no drop is inserted, and on that path the resource simply leaks
    //
    // making it work needs a runtime drop flag per conditionally-moved local, which is real machinery
    // and no part of the chapter. so a conditional move of an *owning* value is rejected rather than
    // silently leaked. a value that owns nothing is unaffected: there is no drop to decide about, so
    // the merge rule stands on its own
    if (!decl->has_type() || !needs_destruction(decl->type())) {
        return;
    }

    _collector.collect_issue<Issue::GenericError>(
        code_ref_for(decl->token_varname), fmt::format(
            "'{}' owns a resource and is moved out of on only one branch, so nothing would destroy it "
            "on the other. Move it on every branch, or after the 'if'.",
            decl->name_full()
        )
    );
}

ExprNode *OwnershipPass::walk_value_edge(ExprNode *expr)
{
    MaterializationScope scope(*this);
    return scope.close(walk_expression(expr));
}

ExprNode *OwnershipPass::walk_expression(ExprNode *expr)
{
    if (expr == nullptr) {
        return nullptr;
    }

    switch (expr->get_node_type()) {
        // **the demand that builds a static's storage.** every read and every write of one reaches
        // here, and the first to do so is what mints the initializer body - so a static nothing names
        // costs nothing, and the alternative, a sweep, would instantiate whatever an untouched
        // `static array<T> $cache` drags in
        case NodeType::n_expr_static_property:
        {
            ensure_static_init(*static_cast<StaticPropertyExprNode *>(expr));
            break;
        }

        case NodeType::n_varref:
        {
            // reading a variable whose value has been handed somewhere else. the whole point of
            // requiring `mv` is that this is a compile error rather than something you discover at
            // runtime
            VarDeclNode *decl = place_root_of(expr);

            if (decl != nullptr && _moved.count(decl) > 0) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(location_of_expression(expr)), fmt::format(
                        "'{}' {} moved out of.",
                        decl->name_full(),
                        _maybe_moved.count(decl) > 0 ? "may have been" : "has been"
                    )
                );
            }
            break;
        }

        case NodeType::n_expr_move:
        {
            // a `mv` in a position that is not one of the four value-arrival sites - inside an
            // arithmetic expression, say. it has no destination to move into, so there is nothing
            // for the transfer to mean
            auto *move = static_cast<MoveExprNode *>(expr);
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(move->token_move),
                "'mv' needs somewhere to move the value to - use it on the whole right-hand side of "
                "an assignment, a call argument, or a return");
            move->operand = walk_value_edge(move->operand);
            break;
        }

        case NodeType::n_expr_call:
        {
            auto *call = static_cast<FunctionCallExprNode *>(expr);

            // **a call forwards, and opens no scope.** it used to bind here, on the grounds that the
            // callee reads through a borrowed address *during* the call and returns - which is true, and
            // is not the whole question. it is false the moment the value the call hands back is *made
            // of* one of those temporaries: `operator [](array<T>&) : T&` returns an address into the
            // very storage the bind would have destroyed, and an `#[implicit]` conversion returns a
            // window into its receiver. both are ordinary library declarations, so no enumeration of
            // shapes could be trusted to catch the next one
            //
            // so the tighter lifetime went, and nothing replaced it: resolve_value_arrival already binds
            // at every destination that *reads* a value, which is where tightness was sound. only a
            // **borrow** argument travels past here, and that is exactly the set that needed to. so
            // `echo sum(Point(3, 4))` still binds around `sum(...)` - echo's own argument reads a value -
            // while `f(&make())` binds at the statement, where nothing can still be pointing into it
            for (size_t i = 0; i < call->arguments.size(); i++) {
                const VarDeclNode *param = nullptr;
                ValueType wanted = ValueType::make_unknown();

                if (call->decl != nullptr && i < call->decl->args.size() && call->decl->args[i] != nullptr) {
                    param = call->decl->args[i];
                    if (param->has_type()) {
                        wanted = param->type();
                    }
                }

                call->arguments[i] = resolve_value_arrival(call->arguments[i], wanted, param, ValueDestination::t_argument);
            }

            // **a print call is the last owner of what it printed.** every other by-value argument hands
            // the value to a callee, and a callee releases its parameter - that is what makes an owning
            // temporary at an argument position safe with no rule here. `echo` has no callee: it is
            // recognised at the call site and lowered inline, so an owning value written straight into one
            // is read, printed, and then referred to by nothing at all. one leaked string per statement,
            // which is invisible until the statement is in a loop
            //
            // **requested rather than bound**, for the reason a call forwards: the value has to outlive the
            // print, and the scope that does is the statement's own value edge. binding here would tear it
            // down before the write it was built for
            if (is_print_call(*call) && !call->arguments.empty() && call->arguments[0] != nullptr
                && !is_place_expression(*call->arguments[0])
                && needs_destruction(call->arguments[0]->result_type())) {
                request_storage_for(expr);
            }

            break;
        }

        case NodeType::n_expr_binary:
        {
            auto *bin = static_cast<BinaryExprNode *>(expr);
            bin->lhs = walk_value_edge(bin->lhs);
            bin->rhs = walk_value_edge(bin->rhs);
            break;
        }

        case NodeType::n_expr_unary:
        {
            auto *un = static_cast<UnaryExprNode *>(expr);
            un->expr = walk_value_edge(un->expr);
            break;
        }

        // three place edges, and the whole of what makes them one: the operand's *storage* is what is
        // being spoken about, so a temporary requested below is still needed here and travels outward
        // to the nearest value edge. that is why `$o->mid()->in->tag` binds once, at the top of the
        // chain, rather than once per `->`
        case NodeType::n_expr_addrof:
        {
            auto *addr = static_cast<AddrOfExprNode *>(expr);
            addr->operand = walk_expression(addr->operand);

            // **a receiver, which is where the second kind of request comes from.** a method takes the
            // address of the value it is called on, so `$o->get()->size()` is `size(&$o->get())` - and
            // the parser wrote that `&` rather than the author. the value needs storage for exactly as
            // long as the call, which is what the flush in the call arm above arranges
            //
            // an `&` the *author* wrote over a non-place never reaches here: the parser refuses it,
            // because a call is not a place whatever it returns and `&5` names nothing at all. **that is
            // the whole marker**, and it is why no flag on the node was needed: an AddrOfExprNode
            // over a non-place is, by construction, one the compiler wrote - CallResolver's borrow
            // coercion, its `#[implicit]` receiver, the parser's method receiver, or emit_resolved_
            // member_call. so this arm only ever sees an address something in the same expression is
            // about to read through - and where that is not true, the destination refuses it (see
            // resolve_value_arrival)
            if (borrow_operand_needs_storage(*addr->operand)) {
                request_storage_for(expr);
            }

            break;
        }

        case NodeType::n_expr_deref:
        {
            auto *deref = static_cast<DerefExprNode *>(expr);
            deref->operand = walk_expression(deref->operand);
            break;
        }

        case NodeType::n_expr_peel:
        {
            auto *peel = static_cast<PointerValueNode *>(expr);
            peel->operand = walk_expression(peel->operand);
            break;
        }

        case NodeType::n_expr_index:
        {
            auto *index_expr = static_cast<IndexExprNode *>(expr);

            // element_call is not reseated, and its type is why: it holds a *call*, and the only
            // replacement this walk performs wraps a member access. so there is nothing it could
            // become that would not also be a type error here
            walk_expression(index_expr->element_call);

            index_expr->base = walk_expression(index_expr->base);
            for (auto *&index : index_expr->indices) {
                index = walk_value_edge(index);
            }
            break;
        }

        case NodeType::n_type_cast:
        {
            auto *cast = static_cast<TypeCastNode *>(expr);
            cast->expr = walk_value_edge(cast->expr);
            break;
        }

        // the nodes this pass inserts itself, and instanceof. a retain wraps a place this walk has
        // already been through, so re-walking it would report a moved-from read twice - but an
        // `instanceof` operand is a read like any other, and its subtree has to be reached or a
        // use-after-move inside it is never seen
        case NodeType::n_expr_instanceof:
        {
            auto *instance_of = static_cast<InstanceOfExprNode *>(expr);
            instance_of->operand = walk_value_edge(instance_of->operand);
            break;
        }

        case NodeType::n_expr_retain:
            break;

        // `strong($w)` **retains**, so its result is one reference nobody holds. it is the same shape a
        // class-returning call is, and it wants the same answer: whoever keeps the value owns that
        // reference, and where nobody does the statement has to give it back
        //
        // no arm of its own beyond the walk - the two forms below register it, because they are where a
        // strong is *produced without being kept*. an upgrade that arrives somewhere - a declaration, a
        // guard binding, an argument - is an ordinary value arrival and already owned
        case NodeType::n_expr_strong:
        {
            auto *strong = static_cast<StrongExprNode *>(expr);
            strong->operand = walk_value_edge(strong->operand);
            break;
        }

        // the two forms that read through a nullable. both branch on a value they may have *upgraded* on
        // the way in - `AST::optional_operand_of` wraps a `weak<T>` operand in a `strong(...)` - and an
        // upgrade is a retain. so the operand is given storage and dropped at the end of the statement,
        // through exactly the machinery a class-returning call's result goes through
        //
        // gated on needing destruction, so the overwhelmingly common case - a nullable that was already
        // a place, or a primitive - binds nothing at all and the tree is left as it was written
        case NodeType::n_expr_optional_chain:
        {
            auto *chain = static_cast<OptionalChainExprNode *>(expr);
            chain->base = walk_expression(chain->base);

            // **unlike `??` below, the base is genuinely discarded**: a chain's result is what the
            // *continuation* produced, so nothing downstream carries the base's reference. that is what
            // makes a temporary the right answer here and the wrong one there
            if (!is_place_expression(*chain->base) && needs_destruction(chain->base->result_type())) {
                request_storage_for(expr);
            }

            // the continuation is rooted at the marker, which stands for the base rather than holding it -
            // so it is walked for the reads *inside* it and cannot ask for the base a second time
            chain->continuation = walk_expression(chain->continuation);
            break;
        }

        // **`??` registers nothing, and that is not an omission.** its result *is* its left side's value on
        // the path where the value was there - so an upgrade's reference flows out of the expression to
        // whoever keeps it, exactly as a class-returning call's does. binding the left side as a temporary
        // and dropping it would take back the very reference the result carries, leaving two names on one
        // object and a count of one
        //
        // so the existing machinery already covers every position: a `??` that *arrives* somewhere is an
        // ordinary value arrival, one that is *read through* gets a temporary from the member-access arm,
        // and one that is *discarded* gets one from bind_discarded_temporary
        case NodeType::n_expr_null_coalesce:
        {
            auto *coalesce = static_cast<NullCoalesceExprNode *>(expr);
            coalesce->lhs = walk_expression(coalesce->lhs);
            coalesce->rhs = walk_value_edge(coalesce->rhs);
            break;
        }

        // **a match owns a declaration and N scopes, so it is walked like the statement it half is.**
        // named here rather than left to `default:` for the reason the arms above are: that one walks
        // nothing, so a subtree reached only through this node would never be walked at all - which is
        // exactly what left an interpolation inside an arm with no scope to materialize its
        // concatenations into, reported as this pass and argument_fit disagreeing
        //
        // the order is the order it runs in: the subject, then each arm's bindings and body, then that
        // arm's value. **the value goes through walk_value_edge**, which opens a MaterializationScope of
        // its own - so a temporary an arm's value makes lives and dies inside that arm, which is the
        // only place it could: an arm is a branch, and a temporary bound at the statement would be
        // acquired on one path and released on all of them
        case NodeType::n_expr_match:
        {
            auto *node = static_cast<MatchExprNode *>(expr);

            if (node->subject != nullptr) {
                walk_statement(make_ref(node->subject));
            }

            for (MatchExprNode::Arm &arm : node->arms) {
                // the arm's own frame: the bindings are its leading declarations, and a block arm's
                // statements follow them. borrows own nothing, so what this frame ends is whatever the
                // arm's *body* declared - which is exactly what a block's frame is for
                if (arm.scope != nullptr) {
                    walk_scope(*arm.scope);
                }

                arm.value = walk_value_edge(arm.value);
            }

            break;
        }

        // a leaf: it stands for a value the enclosing chain already evaluated and owns
        case NodeType::n_expr_chain_base:
            break;

        // a leaf: an allocation has no operand, only the class type it was synthesized for
        case NodeType::n_expr_class_alloc:
            break;

        case NodeType::n_member_access:
        {
            auto *access = static_cast<MemberAccessNode *>(expr);
            auto &base = access->get_base_node();

            if (base.has() && base.is_expression_node()) {
                // **reseated**, the way AST::OperatorRewriter's arm is and AST::PointerAdjuster's is
                // not: a base can be replaced here, and the reference has to follow it
                ExprNode *walked = walk_expression(base.unsafe_ptr<ExprNode>());
                base = NodeReference(walked->get_node_type(), walked);

                // **a base with no storage of its own needs some** - `$o->mid()->tag`, where the value
                // the call handed back is nowhere a `->` can reach into
                //
                // recorded, not rewritten. nothing between here and the flush reads the base, and the
                // two positions that *refuse* a temporary instead of binding one then leave the tree
                // exactly as it was written
                if (member_base_needs_storage(*walked)) {
                    request_storage_for(access);
                }
            }

            break;
        }

        default:
            // literals, nulls, operators: nothing owns anything
            break;
    }

    return expr;
}

ExprNode *OwnershipPass::resolve_value_arrival(
    ExprNode *expr,
    const ValueType &wanted,
    const VarDeclNode *param,
    ValueDestination destination
)
{
    // **a borrow destination does not read the value, it keeps the address.** so this edge is the
    // wrong place to destroy the temporary. Which of the three answers that means depends on how long
    // the destination holds on:
    //
    //  - an **argument** hands the address to a callee that reads through it and returns, so the
    //    request travels one step further out. this position *forwards*, which is spelled by opening
    //    no scope at all - and it is why `use($b->make()->node)` works, and the receiver
    //    `$o->get()->size()` with it
    //  - a **declaration, an assignment, a return or an initialization** keeps it past the statement
    //    entirely, and no lifetime a temporary can have would be long enough. refused
    //  - anything else *reads* the value, so this edge outlives every request below it. bound
    //
    // Asked of `wanted` rather than of the parameter, so all four destinations answer through one
    // rule. That is also what makes it hold whether or not AST::CallResolver has addressed the
    // argument yet - a different pass in the same fixpoint, which may not have run
    if (wanted.is_pointer() && destination == ValueDestination::t_argument) {
        return arrive_value(expr, wanted, param, destination);
    }

    if (wanted.is_pointer()) {
        // one rule, worded as the **source** spells it: the author's own `&`, or a borrow the
        // destination asked for and AST::CallResolver would have inserted. asked of `expr` rather than
        // of what arrive_value hands back, which is what the wording means and is also the only order
        // a scope allows - a refusal states its reason when it opens. the two answer alike on every
        // path that can reach here: the wrappings arrive_value adds are a retain, which takes a place,
        // and an interface cast, which needs a non-pointer destination
        const bool addressed = expr != nullptr && expr->get_node_type() == NodeType::n_expr_addrof;

        MaterializationScope scope(*this,
            addressed ? "the address of" : "a borrow of",
            "would point into a value destroyed at the end of this statement");

        return scope.close(arrive_value(expr, wanted, param, destination));
    }

    // the close is **last** on purpose: arrive_value decides copy-or-move while the expression is
    // still the place it was written as, so a class-typed member read off a temporary is wrapped in a
    // retain by the ordinary copy rule - and only then does the temporary that owns the storage close
    // over the retain. retain-then-release, with no arm here that knows a temporary exists
    MaterializationScope scope(*this);
    return scope.close(arrive_value(expr, wanted, param, destination));
}

ExprNode *OwnershipPass::arrive_value(
    ExprNode *expr,
    const ValueType &wanted,
    const VarDeclNode *param,
    ValueDestination destination
)
{
    if (expr == nullptr) {
        return nullptr;
    }

    if (wanted.is_interface()) {
        // **wherever a class is about to be erased, its deinit has to exist** - and this is the last place
        // the concrete class is known. an erased value's release reaches the class's release thunk through
        // its vtable, and that thunk runs the deinit when the count hits zero; without one it frees the
        // block and the destructor never fires
        //
        // at the top rather than in the t_retain arm below, because a widening is not always a retain: a
        // *call result* is already one reference nobody else holds and takes an early return further down,
        // so `Drawable $d = Circle(1.0);` - a program whose only handle is erased - skipped that arm and
        // silently tore the object down without its destructor
        //
        // asked **under** the implicit casts, because the widening usually arrives as one and the cast's
        // own type is the interface, which names no class to give a deinit to
        const ExprNode *widened = strip_implicit_casts(expr);
        const ValueType source = widened != nullptr ? widened->result_type() : expr->result_type();

        if (source.is_class()) {
            ensure_deinit(source, location_of_expression(expr));
        }
    }

    const bool explicit_move = expr->get_node_type() == NodeType::n_expr_move;

    if (explicit_move) {
        auto *move = static_cast<MoveExprNode *>(expr);

        VarDeclNode *source = whole_variable_moved(move->operand);

        if (source == nullptr) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(move->token_move),
                "'mv' can only move a whole variable - moving a field or an element out of a value "
                "is not supported yet");
        }
        else {
            // the read happens before the move: `$b = mv $a` on an already-moved `$a` is a
            // use-after-move, not a second transfer
            move->operand = walk_expression(move->operand);
            _moved.insert(source);
            _maybe_moved.erase(source);
        }

        // the marker's whole job is done. from here the tree is the plain place expression, and
        // nothing about the move survives except the absence of the copy diagnostic below
        _changed = true;
        return move->operand;
    }

    // a `mv` parameter given a place the caller did not mark. the error is the point of the
    // annotation: a function that quietly swallowed its argument would be indistinguishable at the
    // call site from one that borrowed it
    //
    // **deliberately is_place_expression and not AST::read_reaches_storage**, which would read as
    // catching a borrow-returning call and does not: an argument whose type needs reconciling arrives
    // here already wrapped in the implicit cast AST::CallResolver inserted, and this gate is asked of
    // the cast. so `consume($rows->at(0))` is not refused - it is *copied*, by the ordinary arrival
    // one level down, which is sound if less strict than the place case beside it
    if (param != nullptr && param->takes_ownership && is_place_expression(*expr)) {
        // reported at the *argument*, not at the parameter: the annotation is the declaration's, but
        // the `mv` that has to be written is the caller's
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(location_of_expression(expr)), fmt::format(
                "'{}' takes ownership of this argument - write 'mv' in front of it, or the value would "
                "be handed over without the call site saying so.",
                param->name_full()
            )
        );

        return walk_expression(expr);
    }

    // **an implicit cast is transparent to value arrival**, and what is under it arrives as its own
    // type.
    //
    // AST::CallResolver wraps an argument in one whenever `is_implicitly_convertible` declines, which
    // for a borrow read into a by-value parameter it always does: this pass runs *inside* the fixpoint,
    // so the argument is still `ptr<const T>` where the parameter says `T`, and AST::PointerAdjuster
    // has not yet written the deref that reconciles them.
    //
    // A cast is `t_materializable`, so the place test below took the non-place early return and the
    // value was handed over with no copy and no retain. Two owners, one reference count, and the first
    // teardown frees what the other still names.
    //
    // The copy belongs *inside* the cast, around the place. A RetainExprNode has to be typed as the
    // value for codegen to move the right count, and a copy constructor call has to be the value's
    // rather than whatever the cast reconciled it to. That is also what makes the interface widening
    // above a consequence of this rule rather than an arm of its own.
    //
    // Idempotent across rounds: next round the operand is a retain or a call, neither of which is a
    // place, so this cannot wrap twice. `param` is not forwarded, because the `mv` rule above has
    // already been asked of the outer expression and the parameter's own type is not what arrives
    // under the cast
    if (TypeCastNode *cast = place_under_implicit_cast(*expr)) {
        cast->expr = arrive_value(
            cast->expr, ValueType::make_mutable(value_type_of(cast->expr->result_type())),
            nullptr, destination);

        return expr;
    }

    expr = walk_expression(expr);

    // "a read that reaches storage is copied, a computed value is moved". a value the program
    // computed - a constructor call, a call returning `T` - is one nobody else holds, so it needs no
    // annotation and leaves nothing behind
    //
    // **a call returning a borrow is on the copying side**, which is the whole of why this asks
    // AST::read_reaches_storage rather than is_place_expression: `string $s = $a->at(0);` reads
    // through the address the call handed back, and what it found is still the array's. answering
    // "non-place, so moved" there is a bitwise copy of a buffer handle with no retain, and the
    // local's scope-exit drop then frees storage the container still names
    //
    // and a copy is only this pass's business when it is not a copy of bytes. which copy this is, is
    // AST::classify_copy - decided once here and dispatched on below, because the arms are separated
    // by the move analysis in between and re-deciding them there is what used to make this ladder a
    // second implementation of the one in ASTCopy.cpp. every other copy in the language still happens
    // the way it always did, with nothing inserted and nothing tracked
    if (!read_reaches_storage(*expr)) {
        return expr;
    }

    // after the place test, not beside it: classifying descends into the type's properties, and a
    // non-place has already left with no copy to make
    const CopyKind copy_kind = classify_copy(wanted);

    if (copy_kind == CopyKind::t_bytes) {
        return expr;
    }

    // a borrow parameter is not a destination at all: nothing changes hands, so a place is exactly
    // what belongs there. the wanted type being a pointer already answers this, since classify_copy
    // answers t_bytes for one - but a coercion may not have run yet, so the receiver of a member call
    // arrives here as a bare place against a struct parameter type
    if (param != nullptr && param->has_type() && param->type().is_pointer()) {
        return expr;
    }

    VarDeclNode *source = whole_variable_moved(expr);

    // the two destinations that move a place without being told to - see ValueDestination
    //
    // **a returned local is moved, not copied.** "the caller's slot is the destination, the local is
    // about to die, so there is nothing to duplicate and nothing left behind to destroy", and no
    // `mv` is written because there is nothing else a returned local could be. this is the rule a
    // constructor rests on: its `$this` is a body-local of value type with an implicit
    // `return $this`, so without this `$a = Buffer(...)` frees the buffer twice - once when the
    // constructor's `$this` goes out of scope and once when `$a` does
    //
    // **an initialization moves too**, which is what lets a struct hold an owner at all: the
    // field-wise constructor's `$this->inner = inner` is the parameter being built into the struct,
    // and marking the parameter moved is what stops it being dropped at the end of the constructor
    // while the struct it now lives in is handed back
    // **and an argument the enclosing `return` was about to destroy anyway.** the third implicit move,
    // and the one that is a proof rather than a property of the position: `return f($x)` ends `$x`'s
    // scope exactly as `return $x` does, so the local is handed to the callee rather than duplicated
    // for it - which for an owning type is a whole allocation per call, and is what
    // `return .ok($out)` was paying twice.
    //
    // AST::handover_reads_in is what decides, once per body and never here: this walk knows what came
    // before an arrival and the question is about what comes after it. `param` is required rather than
    // implied, because a call with no declaration - `echo` - has no parameter to hand anything to and
    // is the last owner of what it printed
    const bool hands_over_to_callee =
        destination == ValueDestination::t_argument
        && param != nullptr
        && _handover_reads.count(expr) > 0;

    const bool moves_implicitly =
        destination == ValueDestination::t_return
        || destination == ValueDestination::t_initialization
        || hands_over_to_callee;

    // ...but only of a place that holds its own value. a place read *through* a borrow is not the owner
    // of anything: `return $src` on a `Box& $src` hands the caller a duplicate of a value that stays
    // alive behind the borrow, so marking the borrow moved would claim an ownership the callee never
    // had - and then nothing destroys the original while the caller destroys the copy. so this is a
    // copy, and it reaches the copy constructor below like any other
    //
    // reachable before copies existed too, where it was a silent bitwise copy of an owning struct and
    // a double free waiting for the second destructor. now it is either a real copy or a located error
    const bool source_owns_its_value =
        source != nullptr && source->has_type() && !source->type().is_pointer();

    if (moves_implicitly && source_owns_its_value) {
        _moved.insert(source);
        _maybe_moved.erase(source);
        return expr;
    }

    // **the copy, dispatched on the one classification made above.** a switch and not a ladder of
    // `if`s, and with no `default:`: a fifth way to copy a value cannot be added to AST::CopyKind
    // without answering it here, which is the whole point of there being one enum. what used to sit
    // here re-asked the copy-constructor lookup to find out whether the type had an answer at all, so
    // the refusal was an arm nothing named - and a disagreement between the two would have been a
    // silently byte-copied owner
    FunctionDeclNode *copy_ctor = nullptr;

    switch (copy_kind) {
        // answered above, ahead of the borrow escape and the move analysis, because a value with
        // nothing to arrange is not this pass's business at all. named here to keep the switch total
        case CopyKind::t_bytes:
            return expr;

        // **a class is copied by retaining it.** this is the one place the two storage classes part
        // ways, and it is the whole of the difference.
        //
        // A struct that owns something cannot be duplicated, because there is no way to say what
        // duplicating the thing it owns would mean. A class value owns a *count*, and one more
        // reference to the same object is exactly what a copy of it is.
        //
        // So where a struct gets the diagnostic below, a class gets a retain, and the destination it
        // arrives at owes the matching release: a local at its scope's end, a by-value parameter at the
        // end of the callee's body, a field when it is overwritten or its owner is torn down.
        //
        // An explicit `mv` still works and is still cheaper - it hands the existing reference over
        // instead of adding one. It is just no longer the only option.
        //
        // Note this is reached for a *place* only. A class-typed call result is already one reference
        // nobody else holds, and the early return above lets it through untouched
        case CopyKind::t_retain:
            return &_current_module->nodes.emplace_back<RetainExprNode>(expr);

        // **and the compiler writes that constructor itself when the answer is not a guess**: a struct
        // whose properties each have a copy of their own is copied by copying each of them. built here
        // rather than checked for as a second kind of copy, so what follows cannot tell a synthesized
        // copy constructor from a written one - the whole difference is who wrote the body, which is why
        // this arm and the one below meet at the same emission past the switch
        case CopyKind::t_synthesizable:
            copy_ctor = ensure_copy_constructor(wanted, location_of_expression(expr));
            break;

        // **a struct says what its copy is by declaring a constructor that takes a borrow of itself.**
        // the type holding the raw pointer is the only one that knows what duplicating it means, and
        // this is it saying so. It is the hole a type that owns a raw pointer has to fill first,
        // and the one the others hang off.
        //
        // Recognised rather than newly spelled, so the explicit `Foo($a)` and this implicit copy are
        // one declaration - one way to copy a value rather than two to keep in step.
        //
        // Nothing downstream needs to know either. The result is a call, so it is not a place, and the
        // callee's implicit `return $this` makes it an owner nobody else holds through the t_return
        // move above.
        //
        // The position of this whole switch among its neighbours is load-bearing. It sits after the
        // implicit moves, because a returned local and an initialization *move*, which is cheaper and
        // always correct - and because a constructor's own `return $this` would otherwise call the copy
        // constructor from inside the copy constructor
        case CopyKind::t_constructor:
            copy_ctor = copy_constructor_for(wanted);
            break;

        // nobody has said what a copy of this would mean. an arm of its own now, rather than whatever was
        // left over once a copy-constructor lookup on the way past came back null
        case CopyKind::t_none:
            reject_uncopyable(expr, wanted, source, destination);
            return expr;
    }

    // the classifier named a constructor for both arms that reach here - t_constructor because it read
    // the slot, t_synthesizable because ensure_copy_constructor has just filled it - so a null is a
    // compiler bug rather than a program error. reporting the author-facing refusal instead would blame
    // them for it, which is exactly the divergence this switch exists to make impossible
    assert(copy_ctor != nullptr && "a copy the classifier named has no constructor to call");

    // the type's own name, positioned at the copy rather than at the declaration: this is the call
    // the author could have written by hand, and a diagnostic about it has to point at where the
    // copy happens
    const TokenReference &at =
        virtual_token(copy_ctor->func_name(), Token::Type::t_identifier, location_of_expression(expr));

    return &emit_resolved_member_call(copy_ctor, at, expr);
}

void OwnershipPass::reject_uncopyable(
    ExprNode *expr,
    const ValueType &wanted,
    const VarDeclNode *source,
    ValueDestination destination
)
{
    // **the one way out that is not about this type at all**, so it is spelled once ahead of the two arms
    // that offer it: whether the whole value can be moved instead is a property of `source` and of nothing
    // else
    const std::string transfer = source != nullptr
        ? fmt::format("Write 'mv {}' to transfer it", source->name_full())
        : std::string("Move the whole value rather than a part of it");

    // **a tagged optional gets its own wording, because the general one names something unspellable.**
    // the refusal below ends in "give '{}' a copy constructor", and there is nowhere to write one for a
    // `string?` - the pair is the compiler's layout. what its author can do is give the *payload* one, or
    // bind the value with `guard` and copy that
    if (wanted.is_wrapped_optional()) {
        const ValueType payload = wanted.optional_payload();

        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(location_of_expression(expr)), fmt::format(
                "'{}' may hold a '{}', which owns a resource and cannot be copied. {}, bind it with "
                "'guard' and copy what that gives you, or give '{}' a copy constructor "
                "('constructor({}& $other)') to say what a copy of the value inside is.",
                wanted.get_type_desciption(),
                payload.get_type_desciption(),
                transfer,
                payload.get_type_desciption(),
                payload.get_type_desciption()
            )
        );

        return;
    }

    // **a `#[unique]` type gets its own wording, and it is the opposite advice.** the general
    // refusal below ends in "give it a copy constructor", which for a type whose whole claim is that
    // one value names its storage is the one thing its author must never do
    // AST::classify_copy's reading of the same flag: asked of any type that can carry it, a class and
    // an interface being refused it where it is written
    if (wanted.has_complex_type() && wanted.get_complex_type()->is_unique) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(location_of_expression(expr)), fmt::format(
                "'{}' is unique: exactly one value may name its storage, so it is moved and never "
                "copied. {}, or take a borrow ('{}&') if it is only being read.",
                wanted.get_type_desciption(),
                transfer,
                wanted.get_type_desciption()
            )
        );

        return;
    }

    // a *part* of a value arriving somewhere by copy, which no wording about `mv` would help with:
    // `mv $doc->body` is rejected too, so there is nothing to suggest. reported at the assignment's
    // own token when the source names no variable at all
    if (source == nullptr) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(location_of_expression(expr)), fmt::format(
                "'{}' owns a resource, so this {} would copy a value that cannot be copied. Give '{}' a "
                "copy constructor ('constructor({}& $other)') to say what a copy is - moving a field or "
                "an element out of a value is not supported yet.",
                wanted.get_type_desciption(),
                describe(destination),
                wanted.get_type_desciption(),
                wanted.get_type_desciption()
            )
        );

        return;
    }

    _collector.collect_issue<Issue::GenericError>(
        code_ref_for(location_of_expression(expr)), fmt::format(
            "'{}' owns a resource and cannot be copied implicitly at this {}. Write 'mv {}' to "
            "transfer ownership, take a borrow ('{}&') if the value is only being read, or give '{}' a "
            "copy constructor ('constructor({}& $other)').",
            wanted.get_type_desciption(),
            describe(destination),
            source->name_full(),
            wanted.get_type_desciption(),
            wanted.get_type_desciption(),
            wanted.get_type_desciption()
        )
    );
}

void OwnershipPass::collect_unwind(size_t floor_frame, std::vector<NodeReference> &out)
{
    out.clear();

    for (size_t i = _frames.size(); i-- > floor_frame; ) {
        collect_frame_drops(_frames[i], out);
    }
}

void OwnershipPass::collect_frame_drops(const Frame &frame, std::vector<NodeReference> &out)
{
    // reverse declaration order: the last thing built is the first thing torn down, so a local
    // holding a borrow of an earlier one is gone before its target is
    for (auto local = frame.locals.rbegin(); local != frame.locals.rend(); ++local) {
        if (_moved.count(*local) > 0) {
            // "a moved-from local is also not destroyed at the end of its scope; its destructor
            // travelled with the value"
            continue;
        }

        std::vector<std::string> path;
        emit_drop(*local, path, (*local)->type(), out);
    }
}

void OwnershipPass::emit_drop(
    VarDeclNode *root,
    std::vector<std::string> &path,
    const ValueType &type,
    std::vector<NodeReference> &out
)
{
    // **the place is built once and handed over**, where each arm below used to build its own. they
    // are the same place - only one arm ever runs - and lifting it is what lets storage that is *not*
    // rooted in a declaration be dropped by the same rules: a static property's global is a place
    // like any other, and had no spelling here while a root was a VarDeclNode
    emit_drop_of_place(make_place(root, path), type, root->token_varname, out);
}

void OwnershipPass::emit_drop_of_place(
    ExprNode *place,
    const ValueType &type,
    const TokenReference &at,
    std::vector<NodeReference> &out
)
{
    // a callable owes one release of its environment and has no properties to walk - so it answers here,
    // before the ComplexType it does not have is asked for. no deinit to ensure either: the environment's
    // teardown is uniform, because a callable's static type never says which environment it holds
    if (type.is_callable()) {
        out.push_back(make_ref(_current_module->nodes.emplace_back<ReleaseNode>(place)));
        _changed = true;
        return;
    }

    // an interface value owes exactly what a class does - one reference less - and for the same reason it
    // owes nothing else: what the object holds is decided at the moment its count reaches zero, by the
    // concrete class's own release thunk. answered here, before the ComplexType below, because an
    // interface *has* one and it is the wrong one to walk: it declares no destructor and no properties,
    // so falling through emitted nothing at all and the object leaked
    //
    // no deinit to ensure at this end - the type does not say which class is inside. that is done at the
    // widening, in resolve_value_arrival, which is the last place it is known
    if (type.is_interface()) {
        out.push_back(make_ref(_current_module->nodes.emplace_back<ReleaseNode>(place)));
        _changed = true;
        return;
    }

    // a weak reference owes one weak release, and **nothing else at all** - not the object's destructor,
    // not the object's properties. it never owned any of that; what it owned is the block staying readable
    //
    // answered here for the interface arm's reason - a weak has no ComplexType, so it would fall out of
    // the walk below and leak the block - and it *returns* for the class arm's reason, which matters more
    // here than anywhere: `class Node { weak<Node> $prev; }` is precisely the shape this whole feature
    // exists to make writable, and descending into the class a weak names would have no bottom
    if (type.is_weak()) {
        out.push_back(make_ref(_current_module->nodes.emplace_back<ReleaseNode>(place)));
        _changed = true;
        return;
    }

    const ComplexType *ct = type.get_complex_type();

    if (ct == nullptr) {
        return;
    }

    // a class owes exactly one thing here: one reference less. **not** its teardown - that belongs to the
    // moment the count reaches zero, which may be now, may be later, and may be from an entirely
    // different scope. the release decides, and the deinit is what it calls when it turns out to be the
    // last. asked for here all the same, because this is a moment the class's identity is known
    //
    // returning here rather than falling through is also what makes `class Node { Node $next; }`
    // terminate: recursing into the properties of a type that can contain itself has no bottom
    if (type.is_class()) {
        ensure_deinit(type, at);

        out.push_back(make_ref(_current_module->nodes.emplace_back<ReleaseNode>(place)));
        _changed = true;
        return;
    }

    // **a struct's teardown is a call, exactly like a class's.** it used to be inlined here - the
    // destructor and then a drop per owning property, minted into whatever scope held the value - and the
    // member accesses that took were the compiler reaching inside a type from outside it, which `private`
    // refused. ensure_deinit answers the one function that tears this value down, and where the body of
    // that function is is its decision rather than this one's
    FunctionDeclNode *tear_down = ensure_deinit(type, at);

    // **a teardown this pass owes and could not write is a defect, and it is said out loud.** every caller
    // reached here through needs_destruction, which for a struct is the same question ensure_deinit
    // answers - so a null here means the layout disagreed with its template, which by this point in the
    // pipeline it cannot legitimately do. and a body is walked exactly once, so the alternatives are both
    // silent: emitting nothing leaks, and deferring never converges, since nothing in the fixpoint
    // refreshes a stale layout
    if (tear_down == nullptr) {
        throw std::runtime_error(
            "ownership: no teardown could be written for '" + type.get_mangled_name()
            + "', whose layout is incomplete. This is a compiler defect, not a source error.");
    }

    emit_teardown_call(tear_down, place, at, out);
}

FunctionCallExprNode &OwnershipPass::emit_resolved_member_call(
    FunctionDeclNode *callee,
    const TokenReference &at,
    ExprNode *place
)
{
    // the node, the receiver's addressing and the settlement are all AST::make_resolved_member_call's -
    // this pass is one of its two callers and adds only the round's progress flag. the place that is
    // already an address here is the `$this` of a synthesized class deinit, declared `Foo&` because a
    // by-value class parameter would own a reference and be released by the very function releasing it
    auto &call = make_resolved_member_call(*_current_module, callee, at, place);

    _changed = true;

    return call;
}

AST::ExprNode *OwnershipPass::receiver_for_teardown(AST::ExprNode *place)
{
    const ValueType type = place->result_type();

    // an address is the deinit's `$this`, already the borrow its parameter wants. **stripped of const
    // there too**, and that is not defensive: the exemption in AST::const_receiver_refusal is spelled for
    // `is_destructor()`, and a synthesized deinit is an ordinary method - so what keeps a teardown out of
    // that rule is this function never handing one a const borrow, and it can only claim that by being
    // total
    if (type.is_pointer()) {
        if (!type.pointee().is_const()) {
            return place;
        }

        return &_current_module->nodes.emplace_back<TypeCastNode>(
            ValueType::make_pointer(ValueType::make_mutable(type.pointee()), false), place, false);
    }

    // a mutable place is what every other drop hands over
    if (!type.is_const()) {
        return place;
    }

    auto &address = _current_module->nodes.emplace_back<AddrOfExprNode>(place);

    return &_current_module->nodes.emplace_back<TypeCastNode>(
        ValueType::make_pointer(ValueType::make_mutable(type), false), &address, false);
}

void OwnershipPass::emit_teardown_call(
    FunctionDeclNode *callee,
    ExprNode *place,
    const TokenReference &at,
    std::vector<NodeReference> &out
)
{
    // spelled as the callee names itself: a written `destructor` is a keyword token, a synthesized
    // `$deinit` an identifier. taken from the declaration rather than decided here, so the two answers
    // ensure_deinit can give arrive at the same site looking like what they are
    const TokenReference &receiver_token = virtual_token(
        callee->func_name(),
        callee->name_token.has_value() ? callee->name_token.value().type() : Token::Type::t_identifier,
        at);

    out.push_back(make_ref(emit_resolved_member_call(
        callee, receiver_token, receiver_for_teardown(place))));
}

void OwnershipPass::emit_destructor_call(
    VarDeclNode *root,
    const std::vector<std::string> &path,
    const ComplexType *ct,
    std::vector<NodeReference> &out
)
{
    FunctionDeclNode *dtor = find_destructor(ct);

    if (dtor == nullptr) {
        return;
    }

    emit_teardown_call(dtor, make_place(root, path), root->token_varname, out);
}

IfStatementNode &OwnershipPass::branch_when_present(
    VarDeclNode *tag_root,
    ScopeNode *body,
    const TokenReference &at
)
{
    // the tag, read as the `bool` property it is. no comparison against `null` is minted: `$this->__has`
    // *is* the question, and an operator lookup plus a bound null node would be two more nodes saying the
    // same thing.
    //
    // no else arm: an absent optional owes nothing, and AST::scope_exit_kind reads a branch with no else as
    // leaving nothing - which is what keeps this out of every control-flow rule
    ExprNode *condition = make_place(tag_root, std::vector<std::string>{ k_optional_has_name });

    return branch_on(condition, body, at);
}

IfStatementNode &OwnershipPass::branch_on(
    ExprNode *condition,
    ScopeNode *body,
    const TokenReference &at
)
{
    auto &branch = _current_module->nodes.emplace_back<IfStatementNode>(condition, body, nullptr);

    // emplace rather than assign: a TokenReference holds its collection by reference and so has no copy
    // assignment
    branch.token_if.emplace(virtual_token("if", Token::Type::t_if, at));

    return branch;
}

IfStatementNode &OwnershipPass::branch_when_case(
    VarDeclNode *tag_root,
    const ComplexType *ct,
    int64_t discriminant,
    ScopeNode *body,
    const TokenReference &at
)
{
    ExprNode *tag = make_place(tag_root, std::vector<std::string>{ k_enum_tag_name });

    // the literal is typed to the discriminant's own primitive rather than left to default to int32:
    // the comparison is performed at AST::binary_operation_type's answer for the pair, and a `uint8`
    // tag beside a signed literal is exactly the shape a defaulted int32 would lose against a uint8
    const ValueType tag_type = ct->get_property_type(k_enum_tag_index);

    auto &literal = _current_module->nodes.emplace_back<LiteralIntExprNode>(
        virtual_token(std::to_string(discriminant), Token::Type::t_integer_literal, at),
        tag_type.get_primitive_type());

    const Operator *equals = _collector.operators.get_operator("==");
    assert(equals != nullptr && "the '==' operator is always predefined");

    auto &op_node = _current_module->nodes.emplace_back<OperatorNode>(
        virtual_token("==", Token::Type::t_unknown, at), equals);

    auto &condition = _current_module->nodes.emplace_back<BinaryExprNode>(&op_node, tag, &literal);

    return branch_on(&condition, body, at);
}

void OwnershipPass::emit_enum_case_drops(
    VarDeclNode *root,
    const ComplexType *ct,
    std::vector<NodeReference> &out,
    const TokenReference &at
)
{
    for (const ComplexType::EnumCase &entry : ct->enum_cases()) {
        std::vector<NodeReference> case_drops;

        // reverse declaration order within the case, which is emit_property_drops' rule and the same
        // rule a frame's locals are dropped by
        for (size_t i = entry.payload_field_count; i > 0; i--) {
            const ComplexType::Property &prop =
                ct->get_property(entry.first_payload_property + i - 1);

            if (!needs_destruction(prop.type)) {
                continue;
            }

            std::vector<std::string> path{ prop.name };
            emit_drop(root, path, prop.type, case_drops);
        }

        // a case that owns nothing reaches no new code at all - not even an empty branch. the same
        // property the optional arm has, and what keeps a plain enum's teardown exactly the nothing it
        // was before payload cases existed
        if (case_drops.empty()) {
            continue;
        }

        auto &then_scope = _current_module->nodes.emplace_back<ScopeNode>();

        for (const NodeReference &drop : case_drops) {
            then_scope.children.push_back(drop);
        }

        out.push_back(make_ref(branch_when_case(root, ct, entry.discriminant, &then_scope, at)));
    }
}

void OwnershipPass::emit_property_drops(
    VarDeclNode *root,
    std::vector<std::string> &path,
    const ComplexType *ct,
    std::vector<NodeReference> &out
)
{
    // a struct that contains an owner is itself an owner, and nothing had to be declared for it
    // inlined here rather than emitted as a synthesized destructor because whether a property needs
    // destroying is not answerable in the parser, where such a declaration would have to be built:
    // a generic property type is still open there
    for (size_t i = ct->property_count(); i > 0; i--) {
        const ComplexType::Property &prop = ct->get_property(i - 1);

        if (!needs_destruction(prop.type)) {
            continue;
        }

        path.push_back(prop.name);
        emit_drop(root, path, prop.type, out);
        path.pop_back();
    }
}

FunctionDeclNode &OwnershipPass::begin_synthesized_decl(const std::string &name, const TokenReference &site)
{
    // concrete by construction: everything this pass synthesizes is built per instantiated type rather
    // than per template, so there is nothing left for the monomorphizer to bind - which is the default
    // `inherited_type_param_count` of 0, left as declared
    auto &decl = _current_module->nodes.emplace_back<FunctionDeclNode>(
        virtual_token(name, Token::Type::t_identifier, site));

    // nobody wrote it, so no module owns its symbol - see AST::function_emission_kind. Set here rather
    // than at each of the two callers, because "this pass built it" is exactly what this function means
    decl.is_implicitly_generated = true;

    decl.body = &_current_module->nodes.emplace_back<ScopeNode>();

    return decl;
}

VarDeclNode &OwnershipPass::add_borrow_parameter(
    FunctionDeclNode &decl,
    const std::string &name,
    const ValueType &borrowed,
    const TokenReference &site
)
{
    auto &param_type = _current_module->nodes.emplace_back<TypeNode>(
        ValueType::make_pointer(borrowed, /*nullable=*/false));

    auto &param = _current_module->nodes.emplace_back<VarDeclNode>(
        virtual_token(name, Token::Type::t_varname, site), &param_type);

    decl.args.push_back(&param);

    return param;
}

void OwnershipPass::publish_synthesized_decl(FunctionDeclNode &decl)
{
    // the file the declaration was *built into*, which for a deinit is the one that declares its type
    // rather than the one whose walk asked. carried here rather than resolved at the drain, which by then
    // has only the round to go on
    _pending_declarations.push_back(PendingDecl{ &decl, _current_file });
    _changed = true;
}

FunctionDeclNode *OwnershipPass::ensure_deinit(const ValueType &type, std::optional<TokenReference> at)
{
    ComplexType *ct = type.get_complex_type();

    if (ct == nullptr) {
        return nullptr;
    }

    if (ct->deinit() != nullptr) {
        return ct->deinit();
    }

    // a value that owns nothing owes no teardown at all: for a class that means its release is a
    // decrement and a free, and codegen skips the call when there is no deinit to make
    if (!needs_deinit(ct)) {
        return nullptr;
    }

    // **a template has no layout to tear down.** its property types still mention its parameters, so
    // needs_destruction cannot answer for them, and set_deinit would make that non-answer final - there
    // is no template_or_self redirect on the slot, deliberately, because every instance needs its own
    if (ct->is_generic() && !ct->is_instantiated()) {
        return nullptr;
    }

    // **an instantiation whose properties are not filled in yet is not answerable either**, and the same
    // permanence applies: an application of a generic can be interned during the declaration pass, before
    // the template's own body has been walked, and is refilled later. answering in that window would
    // build a body that drops nothing and make it the final answer, so wait for the round in which the
    // layout agrees with its template. AST::body_is_concrete refuses to walk a body that would ask
    if (ct->is_instantiated() && ct->property_count() != ct->template_or_self()->property_count()) {
        return nullptr;
    }

    // **the destructor is the whole teardown, so it *is* the deinit** - nothing to synthesize, and the
    // drop site calls what the author wrote. `mem::buffer<T>` is the live example, and every array, string
    // and map holds one, so this is the difference between one call at a teardown and three.
    //
    // a struct only, and the reason is which end reads the answer. a struct's teardown is reached through
    // a call site the monomorphizer rewires, so naming the *template's* destructor here is ordinary - it
    // is what emit_destructor_call has always done. a class's is reached from codegen, through the slot,
    // which needs a concrete symbol: filling it with what find_destructor answers for an instantiation
    // would hand the release thunk a declaration that has no symbol at all
    if (!ct->is_class_kind() && !properties_need_destruction(ct)) {
        return find_destructor(ct);
    }

    // **where the declaration is written: at the type, not at the drop that asked for it.** a deinit is
    // shared by every teardown of the type, so the first one to need it is the worst possible author -
    // the body's DISubprogram would take its file from the owner type and its line from a virtual token
    // in another file, and the body's own call nodes would land in whichever module's walk got there
    // first, which build_function_maps' arena sweep turns into a different function order in that
    // module's object. absent only for a compiler-minted anonymous layout, which is a closure
    // environment and therefore a class, and for those the walking file is the honest answer
    const TypeHomeScope home(*this, ct);

    std::optional<TokenReference> site = at;

    // the type's own name token, which a placed home always carries. emplace rather than assign: a
    // TokenReference holds its collection by reference and so has no copy assignment
    if (home.home() != nullptr) {
        site.emplace(home.home()->decl->name_token.value());
    }

    // nowhere to write it and nobody asking from a real line - which is only synthesize_pending_class_deinits
    // sweeping an anonymous layout, and for those the demand-driven ask carries the drop's own token
    if (!site.has_value()) {
        return nullptr;
    }

    return build_deinit(*ct, site.value());
}

OwnershipPass::TypeHomeScope::TypeHomeScope(OwnershipPass &pass, const ComplexType *ct)
    : _pass(pass), _previous_module(pass._current_module), _previous_file(pass._current_file)
{
    const auto found = pass._type_module.find(ct->template_or_self());

    // **a home is only a home if it can position a body**, which is the declaration and its name token
    // together. absent for a compiler-minted anonymous layout - a closure environment, and therefore a
    // class - and for those the walking file is the honest answer, so nothing is swapped at all
    if (found == pass._type_module.end() || found->second.module == nullptr
        || found->second.decl == nullptr || !found->second.decl->name_token.has_value()) {
        return;
    }

    // the file the type was declared in, so the body's file and its line agree. only when nothing
    // could place the declaration does the module's first file stand in
    File *home_file = found->second.file != nullptr
        ? found->second.file
        : found->second.module->files().first();

    // a module with no file, or one whose root the body pass never built. nothing can be published
    // into it, so the asking site stays the answer and `home()` keeps saying so
    if (home_file == nullptr || home_file->root == nullptr) {
        return;
    }

    _home = &found->second;
    _pass._current_module = found->second.module;
    _pass._current_file = home_file;
}

OwnershipPass::TypeHomeScope::~TypeHomeScope()
{
    _pass._current_module = _previous_module;
    _pass._current_file = _previous_file;
}

void OwnershipPass::ensure_static_init(StaticPropertyExprNode &node)
{
    ComplexType *ct = node.owner.get_complex_type();

    if (ct == nullptr || node.decl == nullptr) {
        return;
    }

    const auto key = std::make_pair(static_cast<const ComplexType *>(ct), node.index);

    if (auto it = _static_inits.find(key); it != _static_inits.end()) {
        node.init = it->second.init;
        node.deinit = it->second.deinit;
        return;
    }

    // **a template's static has no storage of its own**, on exactly ensure_deinit's terms: its declared
    // type still mentions the owner's parameters, so neither the layout nor the teardown is answerable -
    // and answering here would make that non-answer permanent. an instantiation is what gets a body
    if (ct->is_generic() && !ct->is_instantiated()) {
        return;
    }

    // written at the type rather than at the access that asked - see TypeHomeScope for why that is a
    // soundness rule and not a tidiness one. the property's own `$name` positions the body either way,
    // so unlike a deinit there is nothing here that a missing home would leave unanswered
    const TypeHomeScope home(*this, ct);
    const TokenReference &site = node.decl->token_varname;

    StaticInit built;

    // the initializer, if one was written. a static with none is simply zero - the global is
    // zero-initialized, which is a defined value and the same one a fresh frame slot would hold
    if (node.decl->init_expr != nullptr) {
        // **the index is part of the name**, or two statics on one type mangle to one symbol -
        // a member's mangled name is its owner, its name and its parameter types, and these take
        // no parameters at all. `$` is the compiler's own namespace, as it is for `$deinit`
        auto &decl = begin_synthesized_decl(
            fmt::format("$static_init{}", node.index), site);

        // the owner is set so AST::enclosing_type_of answers for this body, which is what lets a
        // `private` static be seated by its own type's initializer - and what keeps
        // CodegenContext::function_file_map's `site_of(owner_type)` fallback able to place it
        decl.owner_type = ct;
        decl.return_type = &_current_module->nodes.emplace_back<TypeNode>(ValueType::make_void());

        // **`Type::$x = <the initializer>;`, and that one statement is the whole design.** it is an
        // ordinary assignment into an ordinary place, so OwnershipPass's own walk of this body inserts
        // whatever copy, retain or drop the initializer's shape calls for, with no rule here about any
        // of it. `is_initialization` because the global is fresh, zero-filled storage: there is no
        // previous value owed an ending
        auto &target = _current_module->nodes.emplace_back<StaticPropertyExprNode>(
            node.token_name, node.owner, node.decl, node.index);

        // **an instantiation clones the initializer; a plain type moves it.**
        //
        // the declaration lives on the *template*, so `Box<int32>` and `Box<bool>` reach one
        // `init_expr` between them - and moving it means the first instantiation to be touched steals
        // it and every later one is seated with nothing at all. that is silent: the storage is
        // zero-initialized, so it reads as a plausible value rather than as a missing one
        //
        // a non-generic owner has exactly one instantiation of itself, so moving is right there - and
        // it is what keeps the written expression out of AST::RecursiveVisitor's reach, which would
        // otherwise walk it again through visit_type_decl, in a context with no enclosing function
        ExprNode *initializer = node.decl->init_expr;

        if (ct->is_instantiated()) {
            TypeSubstitution subst = TypeSubstitution::positional(
                ct->template_or_self()->type_parameters, ct->instantiation_args);

            CloneContext cc(_current_module->nodes, subst, _collector.type_registry);
            initializer = static_cast<ExprNode *>(node.decl->init_expr->clone(cc));
        }

        auto &assign = _current_module->nodes.emplace_back<AssignNode>(
            &target, initializer, site);

        assign.is_initialization = true;

        decl.body->children.push_back(AST::make_ref(assign));

        publish_synthesized_decl(decl);
        built.init = &decl;

        // the template's copy stays where it is for the next instantiation to clone; a plain type's
        // is now owned by the body above, and leaving it on the declaration as well would put one
        // subtree in the tree twice
        if (!ct->is_instantiated()) {
            node.decl->init_expr = nullptr;
        }
    }

    // **the teardown, for a static whose type owes one** - the same drop every other owning value
    // gets, over a place that happens to be a global rather than a slot. emit_drop_of_place is what
    // makes that sentence true: a class static gets its release, a struct static gets its deinit call,
    // and a `weak` static gets its weak release, with no arm here about any of them
    //
    // **when** it runs is this subsystem's own answer, and the one thing a local does not need:
    // Compiler::LLVM::StaticStorageCodegen pushes this function onto an intrusive chain as the value
    // is seated, and `main`'s epilogue walks it - LIFO, so reverse of initialization
    if (needs_destruction(node.decl->type())) {
        auto &decl = begin_synthesized_decl(
            fmt::format("$static_deinit{}", node.index), site);

        decl.owner_type = ct;
        decl.return_type = &_current_module->nodes.emplace_back<TypeNode>(ValueType::make_void());

        auto &place = _current_module->nodes.emplace_back<StaticPropertyExprNode>(
            node.token_name, node.owner, node.decl, node.index);

        place.init = built.init;

        std::vector<NodeReference> drops;
        emit_drop_of_place(&place, node.decl->type(), site, drops);

        for (auto &drop : drops) {
            decl.body->children.push_back(drop);
        }

        publish_synthesized_decl(decl);
        built.deinit = &decl;
    }

    _static_inits[key] = built;

    node.init = built.init;
    node.deinit = built.deinit;
}

FunctionDeclNode *OwnershipPass::build_deinit(ComplexType &type, const TokenReference &site)
{
    ComplexType *ct = &type;

    // **`$deinit`, not `deinit`.** the name reaches AST::mangle_function_name through func_name(), and a
    // member's mangled name is its owner segment, its name and its parameter types - so a user's
    // `function deinit()` on this very type mangled to the same symbol and TypeLowering threw "this is a
    // name mangling defect, not a source error" at a program that had done nothing wrong. what makes
    // `destructor` safe there is that it is a keyword token nobody can spell as a name, and `deinit` never
    // was one; `$` is the compiler's own namespace for exactly this - `$this`, `$__it`, `$__env`, `$__temp1`
    auto &decl = begin_synthesized_decl("$deinit", site);

    // **a hint, because a teardown is a call at every scope exit.** the body is one or two calls with no
    // branches, so what an optimized build wants is the sequence a drop site used to hold inline - and
    // AST::function_emission_kind already answers t_odr_shared for anything this pass builds, so the
    // definition is in the same module as every call to it and the inliner needs no whole-program merge.
    // spelled here rather than in begin_synthesized_decl: a copy constructor is a call the *program* makes
    decl.is_inline = true;

    // a member of the type, which is what gives the mangled name its owner segment - mangled_token()
    // already carries the namespace and, for an instantiation, the type arguments. the namespace is
    // deliberately left null: the owner segment already qualifies it, and ComplexType holds its
    // namespace as a const pointer
    //
    // **it is also the whole of the fix for a private owning property.** the member accesses this body
    // holds are the ones a drop used to mint in whatever scope held the value, where AST::enclosing_type_of
    // had no type to answer with and `private` refused the compiler's own teardown. an owner here answers
    // it, and AST::can_reach_private_member needs no arm for a synthesized body
    decl.owner_type = ct;
    decl.member_kind = MemberKind::t_method;

    decl.return_type = &_current_module->nodes.emplace_back<TypeNode>(ValueType::make_void());

    // a borrow keeps nothing alive, which is exactly right for a function that runs when nothing is
    // keeping it alive any more: a by-value class parameter would be released at the end of this very
    // body - the release that got us here, recursing forever
    //
    // **the type's own, not the use that was dropped.** the two differ whenever the drop that asked for
    // this deinit was over a *use* carrying a per-level flag - a `Foo?` local, a `const Foo` property -
    // and this declaration belongs to the type, not to whichever use happened to be dropped first.
    // building `$this` as `Foo?&` made it fail to match the destructor's own `Foo&` receiver, and the
    // shape of the failure is why it is worth spelling: nothing here is wrong, one coercion far away
    // simply has no rule, and reverse-declaration drop order decided which use reached this line first
    auto &this_decl = add_borrow_parameter(decl, "$this", ValueType::make_complex(ct), site);

    ScopeNode &body = *decl.body;

    // published *before* the body is built, not after. building it drops every owning property, and a
    // property asks for its own type's deinit - so `class Node { Node $next; }` would ask for its own,
    // find none, and recurse forever. the same reason TypeRegistry interns an instantiation before
    // substituting its properties
    ct->set_deinit(&decl);

    // the whole teardown, and the only place either half is emitted: the type's own destructor first,
    // then each owning property in reverse declaration order. a destructor is written to release what the
    // value itself owns and may well read a field while doing so
    std::vector<NodeReference> statements;
    std::vector<std::string> path;

    emit_destructor_call(&this_decl, path, ct, statements);

    // **an enum drops the payload of the case it is holding, and no other**, which is the one thing its
    // teardown does that a struct's does not - the enum-shaped reading of the line the tagged optional
    // owns below.
    //
    // a different walk rather than emit_property_drops plus a wrapper, because the grouping *is* the
    // difference: a struct's properties are all live together and are dropped in one reverse pass, an
    // enum's are live one case at a time and are dropped in one guarded group each. the destructor call
    // above stays unguarded either way - it is the whole value's teardown, whatever case that value is
    if (ct->is_enum_kind()) {
        emit_enum_case_drops(&this_decl, ct, statements, site);
    }
    else {
        emit_property_drops(&this_decl, path, ct, statements);
    }

    // **a tagged optional destroys its payload only when it has one**, which is the one thing its teardown
    // does that a struct's does not - and the only reason the pair is a layout at all is that it is
    // expressible here, once per type, instead of at every drop site.
    //
    // the work itself needs no arm: `__has` is a `bool` so emit_property_drops skips it, `__value` drops
    // through the ordinary recursion, and the anonymous layout has no destructor for emit_destructor_call
    // to find. so this collects what a struct would and *wraps* it, which is also why a payload that owns
    // nothing reaches no new code at all - there is nothing to wrap
    if (ct->is_optional && !statements.empty()) {
        auto &then_scope = _current_module->nodes.emplace_back<ScopeNode>();

        for (const auto &statement : statements) {
            then_scope.children.push_back(statement);
        }

        statements.clear();
        statements.push_back(make_ref(branch_when_present(&this_decl, &then_scope, this_decl.token_varname)));
    }

    for (const auto &statement : statements) {
        body.children.push_back(statement);
    }

    publish_synthesized_decl(decl);

    return &decl;
}

FunctionDeclNode *OwnershipPass::ensure_copy_constructor(const ValueType &type, const TokenReference &site)
{
    // the single gate, and it is both halves of the question: whether there is a body to write at all,
    // and the idempotency that builds this once per type, at the first copy that needs it - a published
    // copy constructor classifies as t_constructor, so the second ask declines and the caller reads the
    // slot the first one filled
    //
    // the caller's switch reaches this from the t_synthesizable arm only, so nothing it would decline
    // arrives - it is a precondition restated, kept because a body built around an assumption held in
    // another file is how the ladder below came to be walked twice
    if (!copy_is_synthesizable(type)) {
        return copy_constructor_for(type);
    }

    ComplexType *ct = type.get_complex_type();

    // the type's own, stripped of whatever per-level flags the *use* that asked for this copy carried -
    // `const Foo`, `Foo?`. build_deinit above has the same rule and the note there says why: a
    // declaration synthesized for a type belongs to the type, and the first use to reach it is decided
    // by walk order rather than by anything meaningful
    const ValueType own_type = ValueType::make_complex(ct);

    // named after the struct, like every constructor - the *template's* name for an instantiation,
    // since `Box<int32>` is what the layout is called and `Box` is what a constructor of it is
    auto &decl = begin_synthesized_decl(ct->template_or_self()->name.value_or(""), site);

    // a constructor, and so deliberately **not** a member: owner_type is what implicit_arg_count()
    // keys on, and args[0] here is the `$other` the caller writes rather than a receiver
    //
    // so unlike the deinit above, the mangled name gets no owner segment - and needs none. the sole
    // parameter is typed `Foo&`, and ValueType::get_mangled_name reaches ComplexType::mangled_token
    // through it, which carries both the namespace path and, for an instantiation, the type
    // arguments. `a::Pair` and `b::Pair` cannot collide, and neither can `Box<int32>` and
    // `Box<float64>`. the namespace is left null for the same reason it is on the deinit: ComplexType
    // holds its own as a const pointer, and nothing reads this one - the declaration is never in an
    // overload set to be found by name
    decl.member_kind = MemberKind::t_constructor;

    auto &self_type = _current_module->nodes.emplace_back<TypeNode>(own_type);
    decl.return_type = &self_type;

    // the borrow AST::is_copy_constructor recognises, which is also what makes the call
    // emit_resolved_member_call builds fit without a cast - that lookup drops const on both sides, so
    // `const Foo&` answers it exactly as `Foo&` would
    //
    // **const wherever the properties allow it**, which is AST::copy_source_may_be_const's whole job and
    // deliberately not a decision taken here: this body only reads `$other`, but the per-property copies
    // it delegates to may not, and a hand-written `constructor(Point& $other)` is the author reserving
    // that right. asking makes the two agree instead of making this one optimistic.
    //
    // it matters because a `const` parameter is what lets a copy be taken out of a const *place* -
    // `$other[$i]` inside stdlib/core/array.eco's `constructor(const array<T>& $other)`, which is how an
    // owning array copies its elements at all. with a mutable parameter AST::borrow_preserves_const
    // refuses that argument, and the whole family of copies out of a const value is unspellable
    const ValueType source_type =
        copy_source_may_be_const(own_type) ? ValueType::make_const(own_type) : own_type;

    auto &other_decl = add_borrow_parameter(decl, "$other", source_type, site);

    ScopeNode &body = *decl.body;

    // published *before* the body is built, the same rule build_deinit follows. the body is
    // walked on a later round rather than here, so nothing recurses through this call - but the
    // invariant is worth keeping unconditional, and it is what stops the `return $this` below from
    // ever being read as a copy of a type whose copy is still being decided
    ct->set_copy_constructor(&decl);

    // `Foo $this;` - a body-local of value type, built by the same owner a written constructor's is
    // (AST::declare_constructor_this), and appended before the field writes below for the reason that
    // owner states: a class carries its heap allocation in this declaration's initializer, and an
    // initializer runs where it was written
    //
    // that a class cannot reach here today - classify_copy answers the retain a step earlier, so no
    // reference kind is ever synthesizable - is deliberately *not* what this relies on. that guard lives
    // in another file, and a body built around an assumption held somewhere else is exactly how this
    // line came to write through a handle that was never allocated
    auto &this_decl = declare_constructor_this(*_current_module, self_type, site);
    body.add_vardecl(this_decl);

    // **a tagged optional copies its payload only when it has one.** the tag is copied either way, and the
    // payload's copy - which for an owning payload is a call to *its* copy constructor - is guarded by the
    // tag, or an absent optional would copy-construct out of storage nothing ever wrote. that reads as a
    // crash inside a library's copy constructor, with the absence nowhere in sight.
    //
    // the same shape build_deinit's optional arm has, and for the same reason: the branch belongs in the
    // one body the type owns rather than at each of the sites that copy one
    ScopeNode *present = nullptr;

    if (ct->is_optional) {
        present = &_current_module->nodes.emplace_back<ScopeNode>();
    }

    // **an enum copies the payload of the case `$other` is holding, and no other** - the optional's rule
    // read one case wider, and needed for its reason exactly: a case that is not live is storage nothing
    // ever wrote, so copy-constructing out of it is a crash inside a library's copy constructor with the
    // case nowhere in sight.
    //
    // a per-property lookup rather than a test inside the loop, so the write loop below stays one pass
    // over the layout in layout order - which is what keeps `__tag`'s write ahead of every payload's and
    // makes the body read in the order it runs
    std::vector<ScopeNode *> case_scope_of_property(ct->property_count(), nullptr);
    std::vector<std::pair<const ComplexType::EnumCase *, ScopeNode *>> case_scopes;

    if (ct->is_enum_kind()) {
        for (const ComplexType::EnumCase &entry : ct->enum_cases()) {
            // a case with no payload has nothing to guard, so it gets no branch at all - the property
            // that says which case it is has already been copied unconditionally
            if (!entry.has_payload()) {
                continue;
            }

            auto &scope = _current_module->nodes.emplace_back<ScopeNode>();
            case_scopes.emplace_back(&entry, &scope);

            for (size_t i = 0; i < entry.payload_field_count; i++) {
                case_scope_of_property[entry.first_payload_property + i] = &scope;
            }
        }
    }

    for (size_t i = 0; i < ct->property_count(); i++) {
        const ComplexType::Property &prop = ct->get_property(i);

        // one place per use rather than one shared `$this` read, the rule make_place spells out: no
        // node may sit in the tree twice
        std::vector<std::string> path{prop.name};

        ExprNode *target = make_place(&this_decl, path);
        ExprNode *source = make_place(&other_decl, path);

        // a pointer property is *bound* here, not written through: the slot has never been seated,
        // so a plain assignment would write through uninitialized memory. `$this->prop:$ = $other->prop`,
        // the same re-seating form the synthesized field-wise constructor spells
        if (prop.type.is_pointer()) {
            target = &_current_module->nodes.emplace_back<PointerValueNode>(
                target, virtual_token(prop.name, Token::Type::t_identifier, site));
        }

        auto &assign = _current_module->nodes.emplace_back<AssignNode>(
            target, source, virtual_token(prop.name, Token::Type::t_identifier, site));

        // fresh storage, so no teardown is owed and a `const` property gets its one legitimate write
        assign.is_initialization = true;

        // and deliberately *not* hands_over_value: `$other` is a borrow this constructor does not own,
        // so its fields are copied - which is the whole point - rather than moved out of it
        if (present != nullptr && i == k_optional_value_index) {
            present->children.push_back(make_ref(assign));
        }
        else if (case_scope_of_property[i] != nullptr) {
            case_scope_of_property[i]->children.push_back(make_ref(assign));
        }
        else {
            body.children.push_back(make_ref(assign));
        }
    }

    // the guard, appended after the tag's own write so the two read in the order they run. the same
    // branch build_deinit's optional arm makes, and the tag is read off `$other` because it is `$other`'s
    // payload the write inside reads
    if (present != nullptr) {
        body.children.push_back(make_ref(branch_when_present(&other_decl, present, site)));
    }

    // and the enum's, one per case that has a payload, for the same reason and read off `$other` for
    // the same one: it is `$other`'s payload each branch copies
    for (const auto &[entry, scope] : case_scopes) {
        body.children.push_back(
            make_ref(branch_when_case(&other_decl, ct, entry->discriminant, scope, site)));
    }

    close_constructor_body(*_current_module, decl, this_decl);

    publish_synthesized_decl(decl);

    return &decl;
}

ExprNode *OwnershipPass::member_place(ExprNode *base, const std::string &name, const TokenReference &at)
{
    return &_current_module->nodes.emplace_back<MemberAccessNode>(
        make_ref(base), virtual_token(name, Token::Type::t_identifier, at));
}

ExprNode *OwnershipPass::optional_payload_place(ExprNode *optional_place, const TokenReference &at)
{
    // an ordinary member access, which is the whole dividend of the pair being a layout: `__value` is a
    // property, so nothing here needs a node kind of its own and every pass already knows what this is
    return member_place(optional_place, k_optional_value_name, at);
}

ExprNode *OwnershipPass::make_place(VarDeclNode *root, const std::vector<std::string> &path)
{
    ExprNode *place = &AST::local_place(*_current_module, *root);

    for (const auto &name : path) {
        place = member_place(place, name, root->token_varname);
    }

    return place;
}

};
