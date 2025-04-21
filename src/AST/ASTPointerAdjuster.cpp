#include "AST/ASTPointerAdjuster.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTNullability.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/NullNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeCastNode.h"
#include "AST/VarDeclNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/TemporaryBindExprNode.h"

namespace AST
{

PointerAdjuster::PointerAdjuster(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

CodeRef PointerAdjuster::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, _current_file, token.make_slice()};
}

void PointerAdjuster::run()
{
    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();
        for (auto &file : module_ptr->files()) {
            _current_file = &file;
            if (file.root) {
                adjust(file.root);
            }
        }
    }
}

ExprNode *PointerAdjuster::strip_peel(ExprNode *expr)
{
    if (expr == nullptr || expr->get_node_type() != NodeType::n_expr_peel) {
        return expr;
    }

    auto *peel = static_cast<PointerValueNode *>(expr);
    ExprNode *operand = peel->operand;

    // `:$` is only meaningful on something transparent to strip. reported here rather than in
    // the parser because a type parameter's pointer-ness is not known until monomorphization
    if (!operand->result_type().is_pointer()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(peel->token_peel),
            "':$' expects a pointer, got '" + operand->result_type().get_type_desciption() + "'");
    }

    // the marker's whole job was to stop as_value() inserting a deref here. with it gone the
    // operand stands as its own place, whose value *is* the pointer
    return operand;
}

ExprNode *PointerAdjuster::as_value(ExprNode *expr)
{
    if (expr == nullptr) {
        return nullptr;
    }

    // a peeled expression is already the value it means - the pointer itself - so it keeps its
    // place and gets no deref. this is the entire implementation of `:$`
    if (expr->get_node_type() == NodeType::n_expr_peel) {
        adjust(expr);
        return strip_peel(expr);
    }

    adjust(expr);

    // only a place holding a pointer needs the deref. an AddrOfExprNode is a pointer too, but
    // it is already the value it means - `&$x` yields an address, it does not read through one
    if (!is_place_expression(*expr) || !expr->result_type().is_pointer()) {
        return expr;
    }

    return &_current_module->nodes.emplace_back<DerefExprNode>(expr);
}

ExprNode *PointerAdjuster::as_value_for(ExprNode *expr, const ValueType &wanted)
{
    // a pointer-shaped destination wants the address, not the thing at it. that is the one
    // rule separating a declaration that *binds* (`ptr<int32> $p = &$a;`) from an assignment
    // that writes *through* (`$p = 20;`), and it is the same rule either way.
    //
    // "the address" means the one that actually fits, though. an expression already a level too
    // deep for the destination - a ptr<ptr<int32>> handed to a ptr<int32> slot, which is what
    // `return $v` is inside `first<T>(ptr<T> $v) : T` once T is itself a pointer - still owes
    // one read. binding it unchanged produced a type the destination could not hold
    if (wanted.is_pointer() && expr != nullptr
        && is_implicitly_convertible(expr->result_type(), wanted)) {
        return adjust_place(expr);
    }

    return as_value(expr);
}

template <typename WantedAt>
void PointerAdjuster::adjust_call_arguments(std::vector<ExprNode *> &arguments, WantedAt wanted_at)
{
    for (size_t i = 0; i < arguments.size(); i++) {
        auto *&arg = arguments[i];

        // an argument already wrapped in an address-of was borrowed deliberately,
        // by the coercion pass or by the user writing `&$x`; leave it as the address
        if (arg != nullptr && arg->get_node_type() == NodeType::n_expr_addrof) {
            auto *addr = static_cast<AddrOfExprNode *>(arg);
            addr->operand = adjust_place(addr->operand);
            continue;
        }

        // otherwise the parameter decides how far to read: a pointer parameter takes
        // the address as it is, anything else reads through. that is also where the
        // generic decay lands - an inferred `T` is not a pointer, so a pointer
        // argument is read to its pointee
        arg = as_value_for(arg, wanted_at(i));
    }
}

ExprNode *PointerAdjuster::adjust_place(ExprNode *expr)
{
    if (expr == nullptr) {
        return nullptr;
    }

    adjust(expr);
    return strip_peel(expr);
}

void PointerAdjuster::bind_null_operand(ExprNode *maybe_null, ExprNode *other)
{
    if (maybe_null == nullptr || other == nullptr) {
        return;
    }

    // **the cheap half of the question first.** only a written null has anything to bind, and asking
    // AST::written_null_of is a tag compare - where `other->result_type()` below walks the other operand's
    // whole subtree, which for a nested binary or a member chain is not free. this is called for both
    // operands of every binary node in the program, and almost none of them is a null
    //
    // one walk, and it owns "is this a null at all" - the raw `n_null` tag is not the question, because an
    // implicit cast the parser or the monomorphizer wrapped around it hides that tag
    NullNode *null_node = written_null_of(maybe_null);

    if (null_node == nullptr || null_node->is_bound()) {
        return;
    }

    // the shared rule: does the other side admit absence? that covers a `ptr<T>`, a `weak<T>` and any `T?`
    // - including the wrapped shapes, whose `== null` is a tag test rather than an address comparison and
    // which therefore need the bound type to know their shape
    //
    // a weak is the *one* thing this admits that the other askers would refuse outright. `$w == null`
    // answers whether the reference was ever taken, not whether the object is still alive - that question
    // is `strong($w)`, because only reading the count can answer it
    //
    // **and a comparison-only widening on top of it**: a non-nullable class handle. it is an address, so
    // `$obj == null` lowers to an icmp over two handles and has always been accepted - even though the
    // answer is now statically known, because a `Foo` that is not a `Foo?` is never absent. narrowing that
    // to a diagnostic is a semantic decision of its own and is deliberately not made here; it is spelled at
    // this call site rather than inside destination_admits_null so the other askers cannot inherit it
    const ValueType other_type = other->result_type();

    if (destination_admits_null(other_type) || other_type.is_class()) {
        null_node->bound_type = other_type;
    }
}

void PointerAdjuster::adjust(Node *node)
{
    if (node == nullptr) {
        return;
    }

    switch (node->get_node_type()) {
        case NodeType::n_scope:
        {
            auto *scope = static_cast<ScopeNode *>(node);
            for (auto &child : scope->children) {
                adjust(child.node());
            }
            break;
        }

        case NodeType::n_func_decl:
        {
            auto *fn = static_cast<FunctionDeclNode *>(node);
            // a template's body is only meaningful once cloned into a concrete instance
            if (!fn->is_generic()) {
                FunctionDeclNode *prev = _current_function;
                _current_function = fn;
                adjust(fn->body);
                _current_function = prev;
            }
            break;
        }

        case NodeType::n_vardecl:
        {
            auto *decl = static_cast<VarDeclNode *>(node);
            // a declaration *binds*: `ptr<int32> $p = &$a;` stores the address, so the
            // initializer keeps its pointer when the declared type is a pointer too
            decl->init_expr = as_value_for(
                decl->init_expr, decl->has_type() ? decl->type() : ValueType::make_unknown());
            break;
        }

        case NodeType::n_assign:
        {
            auto *assign = static_cast<AssignNode *>(node);
            // the target goes through as_value like any other read: a plain pointer target
            // gains the deref that write-through means, while `$p:$` keeps the slot it names
            // that is the whole difference between writing through a pointer and re-seating it
            assign->target = as_value(assign->target);

            // the value is then read to fit whatever storage the target turned out to name
            // for `ptr<ptr<int>> $out`, `$out = $target` writes the caller's *pointer*, so the
            // value keeps its address instead of being read through
            assign->value_expr = as_value_for(
                assign->value_expr,
                assign->target != nullptr ? assign->target->result_type() : ValueType::make_unknown());

            // and the old value's teardown, which is a scope of ordinary destructor calls hanging off
            // this statement rather than sitting in the enclosing one's children. not cosmetic: a
            // drop's receiver is `AddrOf(place)`, which the call arm below routes through adjust_place,
            // so a member-path place with a pointer base would otherwise silently lose its deref
            adjust(assign->teardown_old);
            break;
        }

        case NodeType::n_expr_binary:
        {
            auto *bin = static_cast<BinaryExprNode *>(node);
            bin->lhs = as_value(bin->lhs);
            bin->rhs = as_value(bin->rhs);

            // null has no type of its own, so in a comparison it takes the other operand's.
            // done here rather than in the parser because the other side's pointer-ness is
            // only settled once the derefs above are in place
            bind_null_operand(bin->lhs, bin->rhs);
            bind_null_operand(bin->rhs, bin->lhs);
            break;
        }

        case NodeType::n_expr_unary:
        {
            auto *un = static_cast<UnaryExprNode *>(node);
            un->expr = as_value(un->expr);
            break;
        }

        case NodeType::n_expr_call:
        {
            auto *call = static_cast<FunctionCallExprNode *>(node);

            adjust_call_arguments(call->arguments, [call](size_t i) {
                if (call->decl != nullptr && i < call->decl->args.size() && call->decl->args[i]->has_type()) {
                    return call->decl->args[i]->type();
                }

                return ValueType::make_unknown();
            });

            break;
        }

        case NodeType::n_expr_indirect_call:
        {
            auto *call = static_cast<IndirectCallExprNode *>(node);

            // the callee is wanted as a *value*: `$p()` over a `ptr<function<...>>` has to read through
            // to the callable before its fn slot can be extracted. the parameter types come off the
            // signature rather than off a declaration, which is the whole difference from a direct call
            const ValueType callee_type = call->callee_type();
            call->callee = as_value_for(call->callee, callee_type);

            if (!callee_type.is_callable()) {
                break;
            }

            const auto &signature = callee_type.signature();

            adjust_call_arguments(call->arguments, [&signature](size_t i) {
                return i < signature.parameter_types.size()
                    ? signature.parameter_types[i]
                    : ValueType::make_unknown();
            });

            break;
        }

        case NodeType::n_expr_closure:
        {
            auto *closure = static_cast<ClosureExprNode *>(node);

            // each captured place is read in the enclosing frame, as a value of the property it fills
            for (size_t i = 0; i < closure->captured_values.size(); i++) {
                const ValueType wanted = closure->environment_type != nullptr
                    ? closure->environment_type->get_property_type(i)
                    : ValueType::make_unknown();

                closure->captured_values[i] = as_value_for(closure->captured_values[i], wanted);
            }

            break;
        }

        case NodeType::n_expr_addrof:
        {
            // "no transparency peeling": the operand of `&` is wanted as a place
            auto *addr = static_cast<AddrOfExprNode *>(node);
            addr->operand = adjust_place(addr->operand);
            break;
        }

        case NodeType::n_expr_deref:
        {
            auto *deref = static_cast<DerefExprNode *>(node);
            deref->operand = adjust_place(deref->operand);
            break;
        }

        case NodeType::n_expr_index:
        {
            auto *index_expr = static_cast<IndexExprNode *>(node);

            // a container's element access is a call by now, and the operands moved into it - so
            // there is nothing here to adjust that adjusting the call does not already reach, and
            // adjusting them here too would wrap each one twice
            if (index_expr->element_call != nullptr) {
                adjust(index_expr->element_call);
                break;
            }

            // the base is wanted as the address it holds, so it keeps its pointer and gets no
            // deref - `$p:$[1]` offsets from $p's address, it does not read through it first
            index_expr->base = adjust_place(index_expr->base);
            for (auto *&index : index_expr->indices) {
                index = as_value(index);
            }
            break;
        }

        case NodeType::n_expr_peel:
        {
            auto *peel = static_cast<PointerValueNode *>(node);
            peel->operand = adjust_place(peel->operand);
            break;
        }

        case NodeType::n_member_access:
        {
            // the base is wanted as a place; `->` reaching through a pointer is gen_place's job.
            //
            // adjusted in place, not reassigned: no shape reachable under a `->` needs replacing.
            // a peel base is rejected in the parser (`$p:$->x`), and the index arm above rewrites
            // its own base edge. anything that does need a replacement has to reseat the reference
            // here - adjust() alone cannot be observed
            auto *access = static_cast<MemberAccessNode *>(node);
            adjust(access->get_base_node().node());
            break;
        }

        case NodeType::n_expr_instanceof:
        {
            // the operand is read as a value: the question is about the object a handle names, and
            // `$this instanceof Foo` inside a method holds a `Foo&`, so without the deref the
            // comparison would be against the slot's address rather than the handle in it
            auto *instance_of = static_cast<InstanceOfExprNode *>(node);
            instance_of->operand = as_value(instance_of->operand);
            break;
        }

        case NodeType::n_expr_temp_bind:
        {
            auto *bind = static_cast<TemporaryBindExprNode *>(node);

            // the temporaries as the declarations they are, through the vardecl arm above: it reads the
            // initializer *for* the declared type, which is what the call this binds needs
            for (VarDeclNode *temp : bind->temporaries) {
                adjust(temp);
            }

            // the body is a read, and the only one that can be spelled here: the destination this node
            // sits at applied its own rule to the *wrapper*, which is not a place, so a body left
            // unadjusted would keep a pointer member's slot where the value was wanted. as_value rather
            // than as_value_for because a pointer-typed body is refused by AST::OwnershipPass - it would
            // hand back an address into storage the teardown below destroys
            bind->body = as_value(bind->body);

            // and the drops, for the assign arm's reason: a drop's receiver is `AddrOf(place)`, which
            // the call arm routes through adjust_place, so a member-path place with a pointer base would
            // otherwise silently lose its deref
            for (auto &drop : bind->teardown) {
                adjust(drop.node());
            }
            break;
        }

        case NodeType::n_expr_retain:
        {
            // likewise a read: the retain touches the count in the block the *handle* names. the
            // ownership pass wrapped a place here, and this is where that place stops being one
            auto *retain = static_cast<RetainExprNode *>(node);
            retain->operand = as_value(retain->operand);
            break;
        }

        case NodeType::n_release:
        {
            // deliberately not adjusted. codegen reads the target's slot itself, because between the
            // declaration and the release an assignment may have re-seated the variable - so the
            // release wants the place, not a read of it. a borrow is never a release target anyway:
            // needs_destruction is false for a pointer
            break;
        }

        // a leaf: an allocation has no operand, only the class type it was synthesized for
        case NodeType::n_expr_class_alloc:
            break;

        case NodeType::n_type_cast:
        {
            auto *cast = static_cast<TypeCastNode *>(node);
            cast->expr = as_value(cast->expr);
            break;
        }

        case NodeType::n_func_return:
        {
            // a return fits its value to the declared return type, exactly as a declaration
            // and an assignment do. a `T&` return hands back the address, so reading through
            // it here would return the pointee where the signature promised the pointer -
            // which llvm's verifier rejects outright ("return type does not match operand")
            auto *ret = static_cast<ReturnNode *>(node);
            ret->expr = as_value_for(
                ret->expr,
                _current_function != nullptr ? _current_function->get_return_type() : ValueType::make_unknown());
            break;
        }

        case NodeType::n_if_statement:
        {
            auto *stmt = static_cast<IfStatementNode *>(node);
            stmt->condition = as_value(stmt->condition);
            adjust(stmt->if_scope);
            adjust(stmt->else_scope);
            break;
        }

        case NodeType::n_while_statement:
        {
            auto *stmt = static_cast<WhileStatementNode *>(node);
            stmt->condition = as_value(stmt->condition);
            adjust(stmt->loop_scope);
            break;
        }

        case NodeType::n_foreach:
            // a transient node AST::ForeachLowering was supposed to have erased - by lowering it, or by
            // discarding it after a refusal. one reaching here would have every deref inside its body
            // silently skipped by the `default:` arm below, and codegen would read the wrong number of
            // levels with no diagnostic. AST::PointerValueNode's contract
            throw std::runtime_error(
                "a 'foreach' survived the monomorphizer's fixpoint - it should have been lowered into "
                "an iterator and a while, or discarded after a refusal");

        default:
            // leaves: literals, operators, var references, types. nothing to rewrite
            break;
    }
}

};
