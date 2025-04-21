#include "AST/ASTOwnership.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTControlFlow.h"
#include "AST/ASTCopy.h"
#include "AST/ASTDestruction.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/GuardNode.h"
#include "AST/ReleaseNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/TypeCastNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/LoopControlNode.h"
#include "AST/TemporaryBindExprNode.h"

#include <fmt/core.h>

namespace AST
{

namespace
{
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
    // runtime index with nothing static to mark unset. both are listed as unspecified in
    // book/concept/ownership_and_moving.md, "Not yet specified"
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

bool OwnershipPass::run_round()
{
    _changed = false;

    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();

        for (auto &file : module_ptr->files()) {
            _current_file = &file;

            if (file.root == nullptr) {
                continue;
            }

            // cleared before the walk, not between its two halves: the file root's own statements ask
            // for synthesized declarations too
            _pending_declarations.clear();

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

            // the declarations this file's walk asked for - class deinits and copy constructors.
            // appended after the loop rather than during it, since that loop is iterating the very
            // vector this appends to. they are picked up on the next round like any other
            // declaration, which is also when their own bodies get walked: a deinit's `$this` is a
            // borrow and owes nothing of its own, and a copy constructor's field-wise assignments
            // are exactly what has to be walked for the retains to appear
            for (FunctionDeclNode *decl : _pending_declarations) {
                file.root->add_funcdecl(*decl);
            }
            _pending_declarations.clear();
        }
    }

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
    for (size_t i = decl.implicit_arg_count(); i < decl.args.size(); i++) {
        VarDeclNode *arg = decl.args[i];

        if (arg != nullptr && arg->has_type() && needs_destruction(arg->type())) {
            owned_params.push_back(arg);
        }
    }

    _frames.push_back(Frame{decl.body, owned_params});
    walk_scope(*decl.body);
    _frames.pop_back();
}

bool OwnershipPass::body_is_concrete(ScopeNode &scope) const
{
    for (auto &child : scope.children) {
        Node *node = child.node();

        if (node == nullptr) {
            continue;
        }

        switch (node->get_node_type()) {
            case NodeType::n_vardecl:
            {
                auto *decl = static_cast<VarDeclNode *>(node);

                // an untyped declaration is one the monomorphizer has not re-derived yet, and a typed
                // one still mentioning a parameter is waiting on a substitution. either way there is
                // no answer to "does this own something" yet
                if (!decl->has_type() || contains_type_param(decl->type())) {
                    return false;
                }
                break;
            }

            case NodeType::n_guard:
            {
                auto *stmt = static_cast<GuardNode *>(node);

                // the binding is a declaration like any other, and the same two things make it not yet
                // answerable: no type re-derived, or one still mentioning a parameter
                if (stmt->decl != nullptr
                    && (!stmt->decl->has_type() || contains_type_param(stmt->decl->type()))) {
                    return false;
                }

                if (stmt->else_scope != nullptr && !body_is_concrete(*stmt->else_scope)) {
                    return false;
                }
                break;
            }

            case NodeType::n_if_statement:
            {
                auto *stmt = static_cast<IfStatementNode *>(node);
                if (stmt->if_scope != nullptr && !body_is_concrete(*stmt->if_scope)) {
                    return false;
                }
                if (stmt->else_scope != nullptr && !body_is_concrete(*stmt->else_scope)) {
                    return false;
                }
                break;
            }

            case NodeType::n_while_statement:
            {
                auto *stmt = static_cast<WhileStatementNode *>(node);
                if (stmt->loop_scope != nullptr && !body_is_concrete(*stmt->loop_scope)) {
                    return false;
                }
                break;
            }

            case NodeType::n_foreach:
                // **an unlowered foreach is never concrete.** it declares `$el` and `$k` with no type,
                // and the iterator declaration it will mint does not exist yet - so a body holding one
                // has nothing this pass can answer. and the wrong answer here is permanent:
                // _processed_functions means a body is walked exactly once, so an early "yes" leaves
                // `$el`, `$__it` and every local the loop body declares with no drop at all
                //
                // AST::ForeachLowering runs earlier in the same round; once it has, this node is gone
                // and the wrapper scope it left behind is answered by the n_scope arm below
                return false;

            case NodeType::n_scope:
                if (!body_is_concrete(*static_cast<ScopeNode *>(node))) {
                    return false;
                }
                break;

            case NodeType::n_func_decl:
                // its own body, resolved as its own declaration. whether *it* is ready says nothing
                // about whether this one is
                break;

            default:
                break;
        }
    }

    return true;
}

void OwnershipPass::walk_scope(ScopeNode &scope)
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

            collect_unwind(_loop_frames.back(), child.get_ptr<LoopControlNode>()->unwind);
        }

        rebuilt.push_back(kept);
    }

    // spliced *before* the question below rather than after. the question reads the scope's statements,
    // and walk_statement is allowed to replace one - a discarded owning temporary comes back as the
    // declaration that now owns it - so asking it of the pre-walk list would answer about a tree that no
    // longer exists
    scope.children = std::move(rebuilt);

    // the scope's own locals, destroyed in reverse declaration order, at the point after its last
    // statement. skipped when control never reaches that point: whatever left already owes every frame's
    // drops and collected them onto its own ReturnNode::unwind, so a second set here is dead tree
    //
    // dead, and not free - it is type-checked, and a drop of a `Box<int32>` local *creates a generic call
    // site* the monomorphizer then instantiates. it also shows up in `-ar`, which is precisely where a
    // duplicated drop is supposed to be diagnosed rather than printed
    //
    // asked of AST::scope_always_exits rather than re-derived here, which is the fix: this used to test
    // the *last child* for `ReturnNode` and nothing else, so a `die` tail, an `if` whose arms both return,
    // and any statement written after a `return` each appended a full duplicate. none of them ever reached
    // codegen - gen_scope stops at the first terminated block - so the tree was wrong and the binary was
    // not, which is the kind of divergence `-ar` exists to make visible
    if (!scope_always_exits(scope)) {
        collect_frame_drops(_frames.back(), scope.children);
    }

    if (own_frame) {
        _frames.pop_back();
    }
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

            // a whole-variable target is *written*, not read, so a moved-from variable being re-seated
            // is not a use-after-move. the arm below already says so - it clears the moved state and
            // notes "the variable is live again from here on" - but the walk got there first and
            // reported the write as a read. any other target shape (`$a->f`, `$a[$i]`) genuinely does
            // read `$a` to find the storage, so it is walked as usual
            const size_t target_mark = _pending_temporaries.size();

            if (whole_variable_moved(assign->target) == nullptr) {
                assign->target = walk_expression(assign->target);
            }

            // writing into a member of a value with no storage of its own: the bytes are destroyed at
            // the end of this statement, so nothing will ever read what was written. refused rather
            // than bound, which is also what keeps AssignNode::target a place - the wrapper is not one
            refuse_pending_temporaries(target_mark,
                "writing to", "would be lost, because the value is destroyed at the end of this statement");

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

            // **a class target releases whatever it held, and codegen orders the sequence.** it cannot
            // be a node with a place of its own the way a struct's teardown is: the release needs the
            // old handle out of the slot codegen has already addressed, and a class target may be
            // `$node->next` or an element, whose index expression must not be evaluated twice. so the
            // flag says *that* the old reference is owed a release and gen_assign says *when* - retain
            // the new value, read the old handle, store, release the old
            //
            // it also lifts the whole-variable restriction below. writing an owning *struct* into a
            // field is the unspecified partial-ownership case, but a class field holds one handle and
            // replacing it is completely defined, which is what makes `$node->next = $other` - and so
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
                    ensure_class_deinit(target_type, location_of_expression(assign->target));
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
                            code_ref_for(assign->token_assign),
                            fmt::format(
                                "'{}' is initialized twice, and '{}' owns a resource - the value the first "
                                "write built would never be destroyed. Build it once, or assign the whole "
                                "variable instead so the old value is torn down.",
                                description,
                                target_type.get_type_desciption()));
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
                        code_ref_for(assign->token_assign),
                        fmt::format(
                            "Cannot assign a '{}' into a field or element - it owns a resource, and "
                            "replacing part of a value is not supported yet. Assign the whole variable, "
                            "or release the old value first.",
                            target_type.get_type_desciption()));
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

            // **the binding is an ordinary value arrival**, and that is the whole reason `guard` needed no
            // ownership rule of its own: `guard Res $r = $maybe` is a copy of a place, so it retains, and
            // `$r` joins the frame's locals and is released at the scope's end like any other
            //
            // asked at the *declared* type - the non-null one - because that is what `$r` holds. the
            // initializer is one level more nullable, which classify_copy answers identically for: a
            // `Res?` and a `Res` are both one reference to one object
            if (decl != nullptr) {
                const ValueType type = decl->has_type() ? decl->type() : ValueType::make_unknown();

                decl->init_expr = resolve_value_arrival(
                    decl->init_expr, type, nullptr, ValueDestination::t_declaration);

                if (needs_destruction(type)) {
                    _frames.back().locals.push_back(decl);
                }
            }

            // the else arm is walked with the moved-set *saved and restored*, exactly as an `if` arm is:
            // it is a branch, and what it moves out of does not reach the code after the guard
            //
            // no merge, though, and that is the difference from an `if`: the arm cannot fall through
            // (Parser::parse_guard refused one that could), so there is no path on which its moves are
            // visible afterwards. taking the union would mark things moved that this statement's
            // continuation can still legitimately read
            if (stmt->else_scope != nullptr) {
                auto before = _moved;
                walk_scope(*stmt->else_scope);
                _moved = before;
            }

            break;
        }

        case NodeType::n_if_statement:
        {
            auto *stmt = static_cast<IfStatementNode *>(node);
            stmt->condition = walk_value_edge(stmt->condition);

            // each arm moves out of its own copy of the state, and the two are merged by union: a
            // variable moved on either side is unset afterwards. reading pessimism into that is the
            // wrong way round - the alternative is a variable whose validity you can only determine
            // by simulating the branch in your head
            auto before = _moved;

            std::unordered_set<const VarDeclNode *> after_if;
            if (stmt->if_scope != nullptr) {
                walk_scope(*stmt->if_scope);
                after_if = _moved;
            }

            _moved = before;

            std::unordered_set<const VarDeclNode *> after_else = before;
            if (stmt->else_scope != nullptr) {
                walk_scope(*stmt->else_scope);
                after_else = _moved;
            }

            _moved = before;
            for (const auto *decl : after_if) {
                if (_moved.insert(decl).second && after_else.count(decl) == 0) {
                    _maybe_moved.insert(decl);
                    report_conditional_move(decl);
                }
            }
            for (const auto *decl : after_else) {
                if (_moved.insert(decl).second && after_if.count(decl) == 0) {
                    _maybe_moved.insert(decl);
                    report_conditional_move(decl);
                }
            }
            break;
        }

        case NodeType::n_while_statement:
        {
            auto *stmt = static_cast<WhileStatementNode *>(node);

            // a value edge, and the one that most needs to be: a temporary bound here is bound and
            // destroyed *inside* the block the condition is evaluated in, once per iteration. hoisting
            // it to the statement would acquire it every turn and release it once
            stmt->condition = walk_value_edge(stmt->condition);

            // a move inside a loop body runs on every iteration, so a variable declared *outside* the
            // loop would be moved out of twice - the second iteration reading a value that is no
            // longer there. the locals the loop declares itself are fine: each iteration gets its own
            if (stmt->loop_scope != nullptr) {
                std::unordered_set<const VarDeclNode *> outer;
                for (const auto &frame : _frames) {
                    outer.insert(frame.locals.begin(), frame.locals.end());
                }

                auto before = _moved;

                // the frame walk_scope is about to push for the body, recorded here rather than inside
                // it: walk_scope does not know whose scope it has been handed, and its `own_frame` test
                // only tells it whether the frame it needs is already there
                _loop_frames.push_back(_frames.size());
                walk_scope(*stmt->loop_scope);
                _loop_frames.pop_back();

                // note _moved is deliberately *not* saved and restored around the body, and a break's
                // moves must not be either: a break goes to the code after the loop, so what it moved is
                // gone there. that is the opposite of the `guard` arm's treatment, whose else block
                // cannot fall through at all
                for (const auto *decl : _moved) {
                    if (before.count(decl) > 0 || outer.count(decl) == 0) {
                        continue;
                    }

                    _collector.collect_issue<Issue::GenericError>(
                        code_ref_for(decl->token_varname),
                        fmt::format(
                            "'{}' is moved out of inside a loop, so the next iteration would move a "
                            "value that is no longer there. Move it after the loop, or declare it "
                            "inside one.",
                            decl->name_full()));
                }
            }
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

VarDeclNode &OwnershipPass::make_temporary(ExprNode *init, const TokenReference &site)
{
    const TokenReference name_token = virtual_token("$__temp", Token::Type::t_varname, site);

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

        default:
            break;
    }

    // the three arms above are exactly the three sites that push onto _pending_temporaries. a fourth
    // reaching here means one was added without an edge, which would otherwise blind-cast
    assert(false && "a pending temporary was requested by a node with no operand edge");
    return {};
}

std::string OwnershipPass::describe_pending(ExprNode *owner) const
{
    // tested on the one owner that *has* a member to name, rather than on the one that has not: the
    // other way round every kind added later falls into the member arm and is cast to something it is not
    if (owner->get_node_type() == NodeType::n_member_access) {
        return fmt::format("its member '{}'",
            static_cast<MemberAccessNode *>(owner)->get_member_name().value());
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
    // ComplexType, so a type that has none has to be answered before it is asked. since todo/A13c a
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
            code_ref_for(location_of_expression(owner)),
            fmt::format(
                "'{}' has no storage of its own, so {} {} {}. Bind it to a variable first.",
                pending_edge(owner).get()->result_type().get_type_desciption(),
                action,
                describe_pending(owner),
                outcome));
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
        code_ref_for(decl->token_varname),
        fmt::format(
            "'{}' owns a resource and is moved out of on only one branch, so nothing would destroy it "
            "on the other. Move it on every branch, or after the 'if'.",
            decl->name_full()));
}

ExprNode *OwnershipPass::walk_value_edge(ExprNode *expr)
{
    const size_t mark = _pending_temporaries.size();
    return bind_pending_temporaries(walk_expression(expr), mark);
}

ExprNode *OwnershipPass::walk_expression(ExprNode *expr)
{
    if (expr == nullptr) {
        return nullptr;
    }

    switch (expr->get_node_type()) {
        case NodeType::n_varref:
        {
            // reading a variable whose value has been handed somewhere else. the whole point of
            // requiring `mv` is that this is a compile error rather than something you discover at
            // runtime
            VarDeclNode *decl = place_root_of(expr);

            if (decl != nullptr && _moved.count(decl) > 0) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(location_of_expression(expr)),
                    fmt::format("'{}' {} moved out of.",
                        decl->name_full(),
                        _maybe_moved.count(decl) > 0 ? "may have been" : "has been"));
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

            // **a borrow argument's temporary dies here, not at its own edge.** the callee reads through
            // the address *during* the call, so the argument edge left its request pending (see
            // resolve_value_arrival) and the call is the innermost thing that outlives every one of them
            //
            // it is also the tightest correct place: the temporary is built immediately before the call
            // and destroyed immediately after it. for a receiver - argument zero, evaluated first - that
            // is exactly where the author wrote it
            const size_t mark = _pending_temporaries.size();

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

            return bind_pending_temporaries(expr, mark);
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
            // the whole marker**, and it is why todo/A13c needed no flag on the node: an AddrOfExprNode
            // over a non-place is, by construction, one the compiler wrote - CallResolver's borrow
            // coercion, its `#[implicit]` receiver, the parser's method receiver, or emit_resolved_
            // member_call. so this arm only ever sees an address something in the same expression is
            // about to read through - and where that is not true, the destination refuses it (see
            // resolve_value_arrival)
            if (borrow_operand_needs_storage(*addr->operand)) {
                _pending_temporaries.push_back(expr);
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
                _pending_temporaries.push_back(expr);
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
                // the call handed back is nowhere a `->` can reach into (todo/A13b)
                //
                // recorded, not rewritten. nothing between here and the flush reads the base, and the
                // two positions that *refuse* a temporary instead of binding one then leave the tree
                // exactly as it was written
                if (member_base_needs_storage(*walked)) {
                    _pending_temporaries.push_back(access);
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
    ExprNode *expr, const ValueType &wanted, const VarDeclNode *param, ValueDestination destination)
{
    // an arrival is a value edge like any other, and the flush is **last** on purpose: arrive_value
    // decides copy-or-move while the expression is still the place it was written as, so a class-typed
    // member read off a temporary is wrapped in a retain by the ordinary copy rule - and only then does
    // the temporary that owns the storage close over the retain. retain-then-release, with no arm here
    // that knows a temporary exists
    const size_t mark = _pending_temporaries.size();
    ExprNode *arrived = arrive_value(expr, wanted, param, destination);

    // **a borrow destination does not read the value, it keeps the address** - so this edge is the
    // wrong place to destroy the temporary, and which of two things that means depends on how long the
    // destination keeps it:
    //
    //  - an **argument** hands the address to a callee that reads through it and returns. the request
    //    travels one step further out, to the call, which outlives every argument of it - so
    //    `use($b->make()->node)` works, and so does the receiver `$o->get()->size()`
    //  - a **declaration, an assignment, a return or an initialization** keeps it past the statement
    //    entirely, and no lifetime a temporary can have would be long enough. refused
    //
    // asked of `wanted` rather than of the parameter, so all four destinations answer through one rule
    // - and so it holds whether or not AST::CallResolver has addressed the argument yet, which is a
    // different pass in the same fixpoint and may not have run
    if (wanted.is_pointer()) {
        if (destination != ValueDestination::t_argument) {
            // one rule, worded as the source spells it: the author's own `&`, or a borrow the
            // destination asked for and AST::CallResolver would have inserted
            const bool addressed =
                arrived != nullptr && arrived->get_node_type() == NodeType::n_expr_addrof;

            refuse_pending_temporaries(mark,
                addressed ? "the address of" : "a borrow of",
                "would point into a value destroyed at the end of this statement");
        }

        return arrived;
    }

    return bind_pending_temporaries(arrived, mark);
}

ExprNode *OwnershipPass::arrive_value(
    ExprNode *expr, const ValueType &wanted, const VarDeclNode *param, ValueDestination destination)
{
    if (expr == nullptr) {
        return nullptr;
    }

    if (wanted.is_interface()) {
        // **the widening usually arrives as an implicit cast**, inserted by AST::CallResolver to reconcile
        // the argument with the parameter - and a cast is not a place, so the retain further down took the
        // non-place early return and never fired. the callee still released its by-value parameter, so the
        // caller gave away a reference it never added and the object was freed while still in use.
        //
        // the retain belongs *inside* the cast, around the class place: RetainExprNode has to be typed as
        // the **class** for codegen to move the right count, and the cast then widens the retained value.
        // idempotent across rounds by the same mechanism the direct case is - next round the operand is a
        // RetainExprNode, which is not a place, so this cannot wrap twice
        if (expr->get_node_type() == NodeType::n_type_cast) {
            auto *cast = static_cast<TypeCastNode *>(expr);

            if (cast->is_implcit && cast->expr != nullptr
                && cast->expr->result_type().is_class()
                && is_place_expression(*cast->expr)) {
                ensure_class_deinit(cast->expr->result_type(), location_of_expression(cast->expr));

                cast->expr = &_current_module->nodes.emplace_back<RetainExprNode>(cast->expr);
                _changed = true;
                return expr;
            }
        }

        // **wherever a class is about to be erased, its deinit has to exist** - and this is the last place
        // the concrete class is known. an erased value's release reaches the class's release thunk through
        // its vtable, and that thunk runs the deinit when the count hits zero; without one it frees the
        // block and the destructor never fires
        //
        // at the top rather than in the t_retain arm below, because a widening is not always a retain: a
        // *call result* is already one reference nobody else holds and takes an early return further down,
        // so `Drawable $d = Circle(1.0);` - a program whose only handle is erased - skipped that arm and
        // silently tore the object down without its destructor
        const ValueType source = expr->result_type();

        if (source.is_class()) {
            ensure_class_deinit(source, location_of_expression(expr));
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
    if (param != nullptr && param->takes_ownership && is_place_expression(*expr)) {
        // reported at the *argument*, not at the parameter: the annotation is the declaration's, but
        // the `mv` that has to be written is the caller's
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(location_of_expression(expr)),
            fmt::format(
                "'{}' takes ownership of this argument - write 'mv' in front of it, or the value would "
                "be handed over without the call site saying so.",
                param->name_full()));

        return walk_expression(expr);
    }

    expr = walk_expression(expr);

    // "a place is copied, a non-place is moved". a non-place - a call result, a constructor call -
    // is a value nobody else holds, so it needs no annotation and leaves nothing behind
    //
    // and a copy of a place is only this pass's business when it is not a copy of bytes. which copy
    // this is, is AST::classify_copy - decided once here and dispatched on below, because the arms are
    // separated by the move analysis in between and re-deciding them there is what used to make this
    // ladder a second implementation of the one in ASTCopy.cpp. every other copy in the language still
    // happens the way it always did, with nothing inserted and nothing tracked
    if (!is_place_expression(*expr)) {
        return expr;
    }

    // after the place test, not beside it: classifying descends into the type's properties, and a
    // non-place has already left with no copy to make
    const CopyKind copy_kind = classify_copy(wanted);

    if (copy_kind == CopyKind::t_bytes) {
        return expr;
    }

    // a borrow parameter is not a destination at all: nothing changes hands, so a place is exactly
    // what belongs there. the wanted type being a pointer already answers this, since
    // needs_destruction is false for one - but a coercion may not have run yet, so the receiver of
    // a member call arrives here as a bare place against a struct parameter type
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
    const bool moves_implicitly =
        destination == ValueDestination::t_return || destination == ValueDestination::t_initialization;

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

    // **a class is copied by retaining it.** this is the one place the two storage classes part ways,
    // and it is the whole of the difference: a struct that owns something cannot be duplicated, because
    // there is no way to say what duplicating the thing it owns would mean - but a class value owns a
    // *count*, and one more reference to the same object is exactly what a copy of it is
    //
    // so where a struct gets the diagnostics below, a class gets a retain, and the destination it
    // arrives at owes the matching release: a local at its scope's end, a by-value parameter at the end
    // of the callee's body, a field when it is overwritten or its owner is torn down. an explicit `mv`
    // still works and is still cheaper - it hands the existing reference over instead of adding one -
    // it is just no longer the only option
    //
    // note this is reached for a *place* only. a class-typed call result is already one reference
    // nobody else holds, and the early return above lets it through untouched
    if (copy_kind == CopyKind::t_retain) {
        return &_current_module->nodes.emplace_back<RetainExprNode>(expr);
    }

    // **a struct says what its copy is by declaring a constructor that takes a borrow of itself.**
    // the type holding the raw pointer is the only one that knows what duplicating it means, and this
    // is it saying so - which is the hole book/concept/ownership_and_moving.md's "Not yet specified"
    // lists first, and the one the others hang off
    //
    // recognised rather than newly spelled, so the explicit `Foo($a)` and this implicit copy are the
    // same declaration and there is one way to copy a value rather than two to keep in step. and
    // nothing downstream needs to know: the result is a call, so it is not a place, and the callee's
    // implicit `return $this` makes it an owner nobody else holds through the t_return move above.
    //
    // the position among its neighbours is all load-bearing. after the class retain, because a class's
    // copy is one more reference and that stays true even when the class also declares a `Foo&`
    // constructor - which builds a *new* object, a different operation. after the implicit moves,
    // because a returned local and an initialization *move*, which is cheaper and always correct - and
    // because a constructor's own `return $this` would otherwise call the copy constructor from inside
    // the copy constructor. and ahead of both rejections below, because with a copy constructor a
    // *field* source is legal and no longer belongs in the first of them
    //
    // **and the compiler writes that constructor itself when the answer is not a guess**: a struct
    // whose owning properties are all classes is copied by retaining each of them. built here rather
    // than checked for as a second kind of copy, so what follows cannot tell a synthesized copy
    // constructor from a written one - the whole difference is who wrote the body
    //
    // t_none skips this: there is nothing to synthesize, and asking anyway re-walked the type's
    // properties at every copy site of every round for the one case that never has an answer
    if (copy_kind == CopyKind::t_synthesizable) {
        ensure_copy_constructor(wanted, location_of_expression(expr));
    }

    if (FunctionDeclNode *copy_ctor = copy_constructor_for(wanted)) {
        // the type's own name, positioned at the copy rather than at the declaration: this is the call
        // the author could have written by hand, and a diagnostic about it has to point at where the
        // copy happens
        const TokenReference &at =
            virtual_token(copy_ctor->func_name(), Token::Type::t_identifier, location_of_expression(expr));

        return &emit_resolved_member_call(copy_ctor, at, expr);
    }

    // a *part* of a value arriving somewhere by copy, which no wording about `mv` would help with:
    // `mv $doc->body` is rejected too, so there is nothing to suggest. reported at the assignment's
    // own token when the source names no variable at all
    if (source == nullptr) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(location_of_expression(expr)),
            fmt::format(
                "'{}' owns a resource, so this {} would copy a value that cannot be copied. Give '{}' a "
                "copy constructor ('constructor({}& $other)') to say what a copy is - moving a field or "
                "an element out of a value is not supported yet.",
                wanted.get_type_desciption(),
                describe(destination),
                wanted.get_type_desciption(),
                wanted.get_type_desciption()));

        return expr;
    }

    _collector.collect_issue<Issue::GenericError>(
        code_ref_for(location_of_expression(expr)),
        fmt::format(
            "'{}' owns a resource and cannot be copied implicitly at this {}. Write 'mv {}' to "
            "transfer ownership, take a borrow ('{}&') if the value is only being read, or give '{}' a "
            "copy constructor ('constructor({}& $other)').",
            wanted.get_type_desciption(),
            describe(destination),
            source->name_full(),
            wanted.get_type_desciption(),
            wanted.get_type_desciption(),
            wanted.get_type_desciption()));

    return expr;
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
    std::vector<NodeReference> &out)
{
    // a callable owes one release of its environment and has no properties to walk - so it answers here,
    // before the ComplexType it does not have is asked for. no deinit to ensure either: the environment's
    // teardown is uniform, because a callable's static type never says which environment it holds
    if (type.is_callable()) {
        out.push_back(make_ref(_current_module->nodes.emplace_back<ReleaseNode>(make_place(root, path))));
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
        out.push_back(make_ref(_current_module->nodes.emplace_back<ReleaseNode>(make_place(root, path))));
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
        out.push_back(make_ref(_current_module->nodes.emplace_back<ReleaseNode>(make_place(root, path))));
        _changed = true;
        return;
    }

    const ComplexType *ct = type.get_complex_type();

    if (ct == nullptr) {
        return;
    }

    // a class owes exactly one thing here: one reference less. **not** its destructor and not its
    // properties - those belong to the moment the count reaches zero, which may be now, may be later,
    // and may be from an entirely different scope. the release decides, and the class's deinit - built
    // out of the same two helpers this function uses below - is what it calls when it turns out to be
    // the last
    //
    // returning here rather than falling through is also what makes `class Node { Node $next; }`
    // terminate: recursing into the properties of a type that can contain itself has no bottom
    if (type.is_class()) {
        ensure_class_deinit(type, root->token_varname);

        out.push_back(make_ref(_current_module->nodes.emplace_back<ReleaseNode>(make_place(root, path))));
        _changed = true;
        return;
    }

    // the type's own destructor first, then its properties: a destructor is written to release what
    // the struct itself owns, and it may well read a field while doing so
    emit_destructor_call(root, path, ct, out);
    emit_property_drops(root, path, ct, out);
}

FunctionCallExprNode &OwnershipPass::emit_resolved_member_call(
    FunctionDeclNode *callee, const TokenReference &at, ExprNode *place)
{
    // the node, the receiver's addressing and the settlement are all AST::make_resolved_member_call's -
    // this pass is one of its two callers and adds only the round's progress flag. the place that is
    // already an address here is the `$this` of a synthesized class deinit, declared `Foo&` because a
    // by-value class parameter would own a reference and be released by the very function releasing it
    auto &call = make_resolved_member_call(*_current_module, callee, at, place);

    _changed = true;

    return call;
}

void OwnershipPass::emit_destructor_call(
    VarDeclNode *root,
    const std::vector<std::string> &path,
    const ComplexType *ct,
    std::vector<NodeReference> &out)
{
    FunctionDeclNode *dtor = find_destructor(ct);

    if (dtor == nullptr) {
        return;
    }

    const TokenReference &receiver_token =
        virtual_token("destructor", Token::Type::t_destructor, root->token_varname);

    out.push_back(make_ref(emit_resolved_member_call(dtor, receiver_token, make_place(root, path))));
}

void OwnershipPass::emit_property_drops(
    VarDeclNode *root,
    std::vector<std::string> &path,
    const ComplexType *ct,
    std::vector<NodeReference> &out)
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

    decl.body = &_current_module->nodes.emplace_back<ScopeNode>();

    return decl;
}

VarDeclNode &OwnershipPass::add_borrow_parameter(
    FunctionDeclNode &decl, const std::string &name, const ValueType &borrowed, const TokenReference &site)
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
    _pending_declarations.push_back(&decl);
    _changed = true;
}

void OwnershipPass::ensure_class_deinit(const ValueType &class_type, const TokenReference &site)
{
    ComplexType *ct = class_type.get_complex_type();

    if (ct == nullptr || ct->deinit() != nullptr) {
        return;
    }

    // a class whose payload owns nothing needs no teardown at all: its release is a decrement and a
    // free, and codegen skips the call when there is no deinit to make
    if (!class_needs_deinit(ct)) {
        return;
    }

    auto &decl = begin_synthesized_decl("deinit", site);

    // a member of the class, which is what gives the mangled name its owner segment - mangled_token()
    // already carries the namespace and, for an instantiation, the type arguments. the namespace is
    // deliberately left null: the owner segment already qualifies it, and ComplexType holds its
    // namespace as a const pointer
    decl.owner_type = ct;
    decl.member_kind = MemberKind::t_method;

    decl.return_type = &_current_module->nodes.emplace_back<TypeNode>(ValueType::make_void());

    // a borrow keeps nothing alive, which is exactly right for a function that runs when nothing is
    // keeping it alive any more: a by-value class parameter would be released at the end of this very
    // body - the release that got us here, recursing forever
    //
    // **the class's own type, not `class_type`.** the two differ whenever the drop that asked for this
    // deinit was over a *use* of the class carrying a per-level flag - a `Foo?` local, a `const Foo`
    // property - and this declaration belongs to the class, not to whichever use happened to be dropped
    // first. building `$this` as `Foo?&` made it fail to match the destructor's own `Foo&` receiver, and
    // the shape of the failure is why it is worth spelling: nothing here is wrong, one coercion far away
    // simply has no rule, and reverse-declaration drop order decided which use reached this line first
    auto &this_decl = add_borrow_parameter(decl, "$this", ValueType::make_complex(ct), site);

    ScopeNode &body = *decl.body;

    // published *before* the body is built, not after. building it emits a release for every
    // class-typed property, and a release asks for that class's deinit - so `class Node { Node $next; }`
    // would ask for its own, find none, and recurse forever. the same reason TypeRegistry interns an
    // instantiation before substituting its properties
    ct->set_deinit(&decl);

    // the payload's teardown, in the same order and by the same code as a struct's: the class's own
    // destructor first, then each owning property in reverse declaration order
    std::vector<NodeReference> statements;
    std::vector<std::string> path;
    emit_destructor_call(&this_decl, path, ct, statements);
    emit_property_drops(&this_decl, path, ct, statements);

    for (const auto &statement : statements) {
        body.children.push_back(statement);
    }

    publish_synthesized_decl(decl);
}

void OwnershipPass::ensure_copy_constructor(const ValueType &type, const TokenReference &site)
{
    // the single gate, and it is both halves of the question: whether there is a body to write at all,
    // and - since it declines a type AST::find_copy_constructor already answers for - the idempotency
    // that builds this once per type, at the first copy that needs it
    //
    // everything it declines still reaches the two rejections below the caller: a type that owns
    // something the compiler has no rule for is a located error, not a silently wrong copy
    if (!copy_is_synthesizable(type)) {
        return;
    }

    ComplexType *ct = type.get_complex_type();

    // the type's own, stripped of whatever per-level flags the *use* that asked for this copy carried -
    // `const Foo`, `Foo?`. ensure_class_deinit above has the same rule and the note there says why: a
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
    // emit_resolved_member_call builds fit without a cast
    auto &other_decl = add_borrow_parameter(decl, "$other", own_type, site);

    ScopeNode &body = *decl.body;

    // published *before* the body is built, the same rule ensure_class_deinit follows. the body is
    // walked on a later round rather than here, so nothing recurses through this call - but the
    // invariant is worth keeping unconditional, and it is what stops the `return $this` below from
    // ever being read as a copy of a type whose copy is still being decided
    ct->set_copy_constructor(&decl);

    // `Foo $this;` - a body-local of value type, exactly as a written constructor's is. first, because
    // the body reads best that way rather than because it has to be: StmtCodegen::gen_scope seats every
    // declaration's storage at scope entry and ScopeNode::clone clones a scope's declarations before its
    // statements, so position decides nothing here. a written constructor's `$this` is still seeded
    // first for a reason this one does not have - see Parser::seat_this_storage, which puts a class's
    // heap allocation in the initializer, and this synthesized declaration has none
    auto &this_decl = _current_module->nodes.emplace_back<VarDeclNode>(
        virtual_token("$this", Token::Type::t_varname, site), &self_type);
    body.add_vardecl(this_decl);

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
        body.children.push_back(make_ref(assign));
    }

    auto &this_result = _current_module->nodes.emplace_back<ReturnNode>(make_place(&this_decl, {}));
    body.children.push_back(make_ref(this_result));

    publish_synthesized_decl(decl);
}

ExprNode *OwnershipPass::make_place(VarDeclNode *root, const std::vector<std::string> &path)
{
    auto &var = _current_module->nodes.emplace_back<VarNode>(root, root->token_varname);
    ExprNode *place = &_current_module->nodes.emplace_back<VarRefNode>(&var);

    for (const auto &name : path) {
        place = &_current_module->nodes.emplace_back<MemberAccessNode>(
            make_ref(place), virtual_token(name, Token::Type::t_identifier, root->token_varname));
    }

    return place;
}

};
