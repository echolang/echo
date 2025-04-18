#include "AST/ASTOwnership.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTCopy.h"
#include "AST/ASTDestruction.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/ReleaseNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/TypeCastNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/WhileStatementNode.h"

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

    // a token to hang a diagnostic on for an expression that names no variable. every place shape
    // carries one somewhere, so this walks to whichever is nearest the surface rather than falling
    // back to the file's first token - a diagnostic at line 1 is worse than none
    const TokenReference &location_of(ExprNode *expr)
    {
        switch (expr->get_node_type()) {
            case NodeType::n_member_access:
                return static_cast<MemberAccessNode *>(expr)->get_member_name();

            case NodeType::n_expr_index:
                return static_cast<IndexExprNode *>(expr)->token_bracket;

            case NodeType::n_expr_peel:
                return static_cast<PointerValueNode *>(expr)->token_peel;

            case NodeType::n_expr_move:
                return static_cast<MoveExprNode *>(expr)->token_move;

            case NodeType::n_expr_call:
                return static_cast<FunctionCallExprNode *>(expr)->token_function_name;

            case NodeType::n_expr_deref:
                return location_of(static_cast<DerefExprNode *>(expr)->operand);

            case NodeType::n_varref:
                return static_cast<VarRefNode *>(expr)->get_var().use_token();

            default:
                assert(false && "no token to locate this expression at");
                return static_cast<MoveExprNode *>(expr)->token_move;
        }
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

    _processed_roots.insert(&root);

    _current_function = nullptr;
    _frames.clear();
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

        if (child.has_type<ReturnNode>()) {
            // a return leaves every enclosing scope at once, so it owes the drops of all of them,
            // innermost frame first. this and the end of a scope are the *only* two insertion
            // points in the language today - `break` and `continue` are lexed but have no parser
            // arm, so `return` is the only early exit. that window closes the day `break` lands,
            // and this loop is what will have to grow an edge-per-exit
            //
            // collected onto the return, not ahead of it: the returned expression may read what is being
            // dropped, and codegen evaluates the expression before running these - see ReturnNode::unwind
            auto *ret = child.get_ptr<ReturnNode>();

            // rebuilt each round rather than appended to: the pass is idempotent by re-deriving, and the
            // fixpoint walks a body more than once
            ret->unwind.clear();

            for (auto frame = _frames.rbegin(); frame != _frames.rend(); ++frame) {
                collect_frame_drops(*frame, ret->unwind);
            }
        }

        rebuilt.push_back(kept);
    }

    // the scope's own locals, destroyed in reverse declaration order. skipped when the scope's last
    // statement was a return, which already emitted them - anything after a return is unreachable
    // (gen_scope stops there), so a second set would be dead code rather than a double free
    const bool ends_with_return =
        !scope.children.empty() && scope.children.back().has_type<ReturnNode>();

    if (!ends_with_return) {
        collect_frame_drops(_frames.back(), rebuilt);
    }

    scope.children = std::move(rebuilt);

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
            if (whole_variable_moved(assign->target) == nullptr) {
                walk_expression(assign->target);
            }

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
                    ensure_class_deinit(target_type, location_of(assign->target));
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

        case NodeType::n_if_statement:
        {
            auto *stmt = static_cast<IfStatementNode *>(node);
            walk_expression(stmt->condition);

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
            walk_expression(stmt->condition);

            // a move inside a loop body runs on every iteration, so a variable declared *outside* the
            // loop would be moved out of twice - the second iteration reading a value that is no
            // longer there. the locals the loop declares itself are fine: each iteration gets its own
            if (stmt->loop_scope != nullptr) {
                std::unordered_set<const VarDeclNode *> outer;
                for (const auto &frame : _frames) {
                    outer.insert(frame.locals.begin(), frame.locals.end());
                }

                auto before = _moved;

                walk_scope(*stmt->loop_scope);

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
            ExprNode *expr = child.is_expression_node() ? child.unsafe_ptr<ExprNode>() : nullptr;
            walk_expression(expr);

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

            break;
        }
    }

    return child;
}

VarDeclNode &OwnershipPass::bind_discarded_temporary(ExprNode *expr)
{
    const TokenReference &site = location_of(expr);

    const TokenReference name_token = virtual_token("$__temp", Token::Type::t_varname, site);

    auto &type_node = _current_module->nodes.emplace_back<TypeNode>(expr->result_type());
    auto &decl = _current_module->nodes.emplace_back<VarDeclNode>(name_token, &type_node);

    // no retain: the value is a non-place, so it is already the one reference nobody else holds -
    // the same rule that lets `Foo $a = Foo();` bind without one
    decl.init_expr = expr;

    _frames.back().locals.push_back(&decl);
    _changed = true;

    return decl;
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

void OwnershipPass::walk_expression(ExprNode *expr)
{
    if (expr == nullptr) {
        return;
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
                    code_ref_for(location_of(expr)),
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
            walk_expression(move->operand);
            break;
        }

        case NodeType::n_expr_call:
        {
            auto *call = static_cast<FunctionCallExprNode *>(expr);

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
            break;
        }

        case NodeType::n_expr_binary:
        {
            auto *bin = static_cast<BinaryExprNode *>(expr);
            walk_expression(bin->lhs);
            walk_expression(bin->rhs);
            break;
        }

        case NodeType::n_expr_unary:
            walk_expression(static_cast<UnaryExprNode *>(expr)->expr);
            break;

        case NodeType::n_expr_addrof:
            walk_expression(static_cast<AddrOfExprNode *>(expr)->operand);
            break;

        case NodeType::n_expr_deref:
            walk_expression(static_cast<DerefExprNode *>(expr)->operand);
            break;

        case NodeType::n_expr_peel:
            walk_expression(static_cast<PointerValueNode *>(expr)->operand);
            break;

        case NodeType::n_expr_index:
        {
            auto *index_expr = static_cast<IndexExprNode *>(expr);
            walk_expression(index_expr->element_call);
            walk_expression(index_expr->base);
            for (auto *index : index_expr->indices) {
                walk_expression(index);
            }
            break;
        }

        case NodeType::n_type_cast:
            walk_expression(static_cast<TypeCastNode *>(expr)->expr);
            break;

        // the nodes this pass inserts itself, and instanceof. a retain wraps a place this walk has
        // already been through, so re-walking it would report a moved-from read twice - but an
        // `instanceof` operand is a read like any other, and its subtree has to be reached or a
        // use-after-move inside it is never seen
        case NodeType::n_expr_instanceof:
            walk_expression(static_cast<InstanceOfExprNode *>(expr)->operand);
            break;

        case NodeType::n_expr_retain:
            break;

        // a leaf: an allocation has no operand, only the class type it was synthesized for
        case NodeType::n_expr_class_alloc:
            break;

        case NodeType::n_member_access:
        {
            auto *access = static_cast<MemberAccessNode *>(expr);
            auto &base = access->get_base_node();
            if (base.has() && base.is_expression_node()) {
                walk_expression(base.unsafe_ptr<ExprNode>());
            }
            break;
        }

        default:
            // literals, nulls, operators: nothing owns anything
            break;
    }
}

ExprNode *OwnershipPass::resolve_value_arrival(
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
                ensure_class_deinit(cast->expr->result_type(), location_of(cast->expr));

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
            ensure_class_deinit(source, location_of(expr));
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
            walk_expression(move->operand);
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
            code_ref_for(location_of(expr)),
            fmt::format(
                "'{}' takes ownership of this argument - write 'mv' in front of it, or the value would "
                "be handed over without the call site saying so.",
                param->name_full()));

        walk_expression(expr);
        return expr;
    }

    walk_expression(expr);

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
        ensure_copy_constructor(wanted, location_of(expr));
    }

    if (FunctionDeclNode *copy_ctor = copy_constructor_for(wanted)) {
        // the type's own name, positioned at the copy rather than at the declaration: this is the call
        // the author could have written by hand, and a diagnostic about it has to point at where the
        // copy happens
        const TokenReference &at =
            virtual_token(copy_ctor->func_name(), Token::Type::t_identifier, location_of(expr));

        return &emit_resolved_member_call(copy_ctor, at, expr);
    }

    // a *part* of a value arriving somewhere by copy, which no wording about `mv` would help with:
    // `mv $doc->body` is rejected too, so there is nothing to suggest. reported at the assignment's
    // own token when the source names no variable at all
    if (source == nullptr) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(location_of(expr)),
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
        code_ref_for(location_of(expr)),
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
    // the receiver is addressed here, exactly as the parser addresses a method's: the parameter is the
    // borrow `Foo&`, and a value ranked against it would be no fit at all
    //
    // unless the place already *is* that address. that happens in exactly one situation - the `$this`
    // of a synthesized class deinit, which is declared `Foo&` because a by-value class parameter would
    // own a reference and be released at the end of the very function doing the releasing. addressing
    // it again would hand the callee a ptr<ptr<Foo>>
    ExprNode *receiver = place;

    const ValueType place_type = place->result_type();
    if (!place_type.is_pointer()) {
        receiver = &_current_module->nodes.emplace_back<AddrOfExprNode>(place);
    }

    auto &call = _current_module->nodes.emplace_back<FunctionCallExprNode>(
        at, std::vector<ExprNode *>{receiver});

    // resolved already: there is no name to look up and no overload set to search. for an
    // instantiation this is the *template's* declaration, and the monomorphizer's next round
    // binds the owner's parameters from the receiver and rewires this call to the instance -
    // which is the whole reason the pass runs inside that fixpoint
    //
    // so this pass is the one other producer of a call that already knows its declaration, and it
    // publishes the state that describes it: choosing is done, fitting the receiver to the callee's
    // borrow parameter is still AST::CallResolver's, in a later round
    call.decl = callee;
    call.settlement = CallSettlement::t_uncoerced;

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
    auto &this_decl = add_borrow_parameter(decl, "$this", class_type, site);

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

    auto &self_type = _current_module->nodes.emplace_back<TypeNode>(type);
    decl.return_type = &self_type;

    // the borrow AST::is_copy_constructor recognises, which is also what makes the call
    // emit_resolved_member_call builds fit without a cast
    auto &other_decl = add_borrow_parameter(decl, "$other", type, site);

    ScopeNode &body = *decl.body;

    // published *before* the body is built, the same rule ensure_class_deinit follows. the body is
    // walked on a later round rather than here, so nothing recurses through this call - but the
    // invariant is worth keeping unconditional, and it is what stops the `return $this` below from
    // ever being read as a copy of a type whose copy is still being decided
    ct->set_copy_constructor(&decl);

    // `Foo $this;` - a body-local of value type, exactly as a written constructor's is, and the
    // *first* child: gen_scope allocas in child order, so a `$this` declared after the statements
    // that read it has no storage yet (see TypeDeclParser, which seeds it for the same reason)
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
