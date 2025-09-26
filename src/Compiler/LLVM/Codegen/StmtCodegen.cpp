#include "Compiler/LLVM/Codegen/StmtCodegen.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/Codegen/ClassCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ASTNullability.h"
#include "AST/ScopeNode.h"
#include "AST/ASTMangler.h"
#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/VarNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/AssignNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ExprNode.h"
#include "AST/TypeCastNode.h"
#include "AST/ReturnNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/GuardNode.h"
#include "AST/LoopControlNode.h"
#include "AST/IfStatementNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/ASTPlaceExpr.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <cassert>
#include <stdexcept>
#include <vector>

namespace Compiler::LLVM
{
void StmtCodegen::gen_scope(AST::ScopeNode &node)
{
    // asked before the slots below rather than only between statements: an alloca is an instruction, and
    // a block that already ends cannot take one - a second terminator in one block fails the verifier.
    // the same question gen_temporary_bind asks for the same reason
    if (_ctx.block_is_terminated()) {
        return;
    }

    // every declaration this scope makes is seated here, before the first statement runs - so its slot
    // exists for the whole scope, which is what stops a declaration's *position* among its siblings from
    // being load-bearing: a statement above it reads a slot that is already there
    //
    // the alloca itself does not land here. CodegenContext::entry_alloca puts it in the function's entry
    // block, so a loop body does not allocate one per iteration; what this sweep places is the *zero-init*,
    // and scope entry is the right height for it because it is the block a loop re-enters - a `Foo $x;`
    // inside one is re-cleared each turn
    //
    // direct children only, for that same reason. a declaration nested in an `if` arm or a loop body
    // belongs to that scope's sweep, and clearing it from here would clear it once instead of per turn
    for (auto &child : node.children) {
        if (child.has_type<AST::VarDeclNode>()) {
            ensure_var_slot(*child.get_ptr<AST::VarDeclNode>());
        }
    }

    for (auto &child : node.children) {

        // skip function declarations
        if (child.has_type<AST::FunctionDeclNode>()) {
            continue;
        }

        // a statement must leave the value stack exactly as it found it. every value pushed by
        // a subexpression belongs to the parent that asked for it, so anything still on the
        // stack here is a leak - and a leak silently feeds the wrong value to a later pop
        const size_t depth_before = _ctx.value_stack.size();

        child.node()->accept(*_ctx.visitor);

        // an expression statement discards its value: `mem::free($p);` is a whole statement even
        // though the call has a result, and libc's memcpy/memset return a pointer nobody wants.
        // this is the one place a pushed value legitimately has no parent to claim it, so drop
        // it here rather than making callees lie about their return type
        if (AST::make_ref(child.node()).is_expression_node()) {
            while (_ctx.value_stack.size() > depth_before) {
                _ctx.value_stack.pop();
            }
        }

        // anything else leaving a value behind is still a genuine codegen bug
        assert(_ctx.value_stack.size() == depth_before && "statement leaked a value onto the stack");

        // the block this statement left the builder in already ends, so everything after it is
        // unreachable and must not be emitted - a second terminator in one block fails the
        // verifier. asked of the block rather than of the statement because a return is not the
        // only thing that terminates one: an `if` whose every arm returns leaves the builder
        // inside the last arm, and the scope-exit drops the ownership pass appends after it would
        // land there. that shape is only reachable at all once a scope owes drops
        if (_ctx.block_is_terminated()) {
            break;
        }
    }
}

llvm::AllocaInst *StmtCodegen::ensure_var_slot(AST::VarDeclNode &node)
{
    // a declaration carrying no type describes no slot. unreachable for a program that got this far -
    // every declaration the parser builds has a type node and AST::TypeChecker refuses an unresolved one -
    // but asked rather than left to type_node()'s assert, because the sweep in gen_scope reaches
    // declarations codegen never used to visit at all: one written after a `return` is now seated too
    if (!node.has_type()) {
        return nullptr;
    }

    // idempotent, and deliberately not by presence alone: var_map is keyed by declaration and never
    // cleared between functions, so a hit that was emitted into *another* llvm::Function is not this
    // scope's slot - reusing it would build a body reading an alloca that does not dominate its uses
    auto found = _ctx.var_map.find(&node);
    if (found != _ctx.var_map.end() &&
        found->second->getFunction() == _ctx.builder->GetInsertBlock()->getParent()) {
        return found->second;
    }

    llvm::Type *type = _ctx.types->get_llvm_type(node.type_node()->type, *_ctx.current_cmp_unit);

    // the slot itself goes to the function's entry block and the zero-init below stays here, which is the
    // one split that makes both right - see CodegenContext::entry_alloca. emitted where the builder stood,
    // a declaration in a loop body allocated a fresh slot every iteration
    llvm::AllocaInst *alloca = _ctx.entry_alloca(type, node.name());

    // store the variable in the map
    _ctx.var_map[&node] = alloca;

    // an aggregate with no initializer used to be left holding `undef`, which a constructor that
    // writes only some of its fields then read back as garbage. it is a prerequisite of scope-exit
    // destruction rather than a tidy-up: a destructor over an undef pointer field frees whatever
    // happened to be in those bytes. only aggregates, and only when nothing else writes the slot -
    // a scalar with an initializer is fully covered by the store gen_var_decl emits
    //
    // a class local is a scalar - one handle - but needs the same treatment for the same reason and
    // one step more urgently: the scope-exit release reads that slot unconditionally, so an
    // uninitialized `Foo $x;` would decrement a count at whatever address was on the stack. null is
    // a legitimate value of the type, and releasing null is a no-op
    //
    // **it stays here, where the alloca above no longer is** - see the split at the top of this function.
    // and not at the *declaration's* position either, where it would zero a value the statements above
    // already built. scope entry is the one height that is right for it
    //
    // **a guard's binding counts as having no initializer here**, and for exactly the reason this
    // zero-init exists. it is only written on the path where the value was there, but the scope-exit
    // release reads the slot on *both* - the else arm's unwind releases it too, because the frame's local
    // list is positional and does not know the arm cannot reach the binding. so on that path the slot
    // would hold whatever was on the stack, and the release would decrement a count at that address
    //
    // null is a legitimate value of every type this applies to, and releasing null is a no-op
    const bool needs_zero_init = type->isAggregateType() || node.type_node()->type.is_class();
    if (needs_zero_init && (!node.init_expr || node.binds_unwrapped)) {
        _ctx.builder->CreateStore(llvm::Constant::getNullValue(type), alloca);
    }

    return alloca;
}

void StmtCodegen::gen_var_decl(AST::VarDeclNode &node)
{
    // gen_scope seated this already for a declaration that is a scope's child, which is all of them a
    // program writes. asked again rather than assumed, because a temporary is *not* a scope's child -
    // gen_temporary_bind visits its declarations directly - and one owner of the slot is what keeps the
    // two paths from disagreeing about whether it has been created
    llvm::AllocaInst *alloca = ensure_var_slot(node);

    if (!alloca) {
        return;
    }

    // the initializer stays at the declaration's position and is *not* hoisted with the slot: it is a
    // statement, and statements run in the order they were written. for a class constructor's `$this`
    // that is load-bearing rather than tidy - AST::declare_constructor_this puts the heap allocation in
    // this initializer, so the declaration still has to precede every field write that stores through it
    if (node.init_expr) {
        node.init_expr->accept(*_ctx.visitor);

        // check that the visited node pushed a value on the stack
        assert(_ctx.value_stack.size() > 0 && "No value on the stack");

        llvm::Value *init_value = _ctx.value_stack.top();
        _ctx.value_stack.pop();

        // the same conversion every assignment and member write uses. this path used to handle
        // only float/double, so an initializer that widened an integer stored the narrow value
        // straight into the wide slot and read back whatever else was in those bytes
        _ctx.builder->CreateStore(
            _ctx.types->coerce_value(init_value, node.init_expr->result_type(), node.type_node()->type, *_ctx.current_cmp_unit),
            alloca);
    }
}

void StmtCodegen::gen_function_decl(AST::FunctionDeclNode &node)
{
    // skip compilation of generic function templates
    if (node.is_generic()) {
        return;
    }

    // sanity checks

    // 1. must have a body
    if (!node.body) {
        // if its an intrinsic function we can skip this
        if (node.intrinsic) {
            return;
        }

        // a builtin has no symbol at all - it is answered in gen_builtin_call at each call site, so
        // there is nothing to emit here. spelled out rather than left to the !is_generic() fallback
        // below, which only happened to cover the generic builtins that existed first
        if (node.is_builtin()) {
            return;
        }

        // an interface requirement has no body by construction - the implementors do, under their own
        // symbols. spelled out beside the builtin above rather than left to the fallback, so the reason
        // is visible where the two other readers of the same predicate are
        if (node.is_interface_requirement()) {
            return;
        }

        // skip instantiated generic functions that don't have bodies yet
        // this is a temporary measure while we implement proper body cloning
        // (is_generic() is exactly !type_parameters.empty(), so one check covers it)
        if (!node.is_generic()) {
            return;
        }

        assert(false);
        throw _ctx.error(fmt::format(
            "Function '{}' has no body associated with it.",
            node.func_name()
        ));
    }

    // track the enclosing function so codegen errors can name it. restored before returning
    AST::FunctionDeclNode *prev_function = _ctx.current_function;
    _ctx.current_function = &node;

    // dump all function names in map
    auto funcid = _ctx.current_cmp_unit->function_table.get_function_id_by_name(AST::mangle_function_name(&node));
    auto func = _ctx.current_cmp_unit->function_table.get_llvm_function(funcid);

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", func);
    _ctx.builder->SetInsertPoint(entry);

    // create the arguments
    // the parameter slots. through entry_alloca like every other slot even though the builder is already
    // standing in the entry block - one owner for "where does a stack slot come from" is worth more than
    // the line it saves here, and it is what keeps this loop from being the counter-example somebody
    // copies into a loop body later
    for (auto &arg : func->args()) {
        arg.setName(node.args[arg.getArgNo()]->name());
        llvm::AllocaInst *alloca = _ctx.entry_alloca(arg.getType(), arg.getName());
        _ctx.builder->CreateStore(&arg, alloca);
        _ctx.var_map[node.args[arg.getArgNo()]] = alloca;
    }

    // a synthesized constructor arrives here like any other function - the struct parser builds its
    // body out of the same nodes a user would write, which is what keeps one implementation of the
    // member write and of the pointer re-seat
    node.body->accept(*_ctx.visitor);

    // add a terminator if the block doesn't already have one
    if (!_ctx.block_is_terminated()) {
        // if the function returns void, add a void return
        if (func->getReturnType()->isVoidTy()) {
            _ctx.builder->CreateRetVoid();
        } else {
            // for non-void functions without explicit return, this is an error
            // but we'll add a dummy return to keep LLVM happy
            llvm::Value *dummy_ret = llvm::UndefValue::get(func->getReturnType());
            _ctx.builder->CreateRet(dummy_ret);
        }
    }

    _ctx.current_function = prev_function;
}

void StmtCodegen::gen_return(AST::ReturnNode &node)
{
    // the drops this return owes, run *after* the returned value is computed - see ReturnNode::unwind.
    // one helper rather than three copies, because every exit below has to run them
    auto emit_unwind = [&]() {
        for (auto &drop : node.unwind) {
            if (drop.has()) {
                drop.node()->accept(*_ctx.visitor);
            }
        }
    };

    // handle returns without an actual extression
    if (node.expr == nullptr) {
        emit_unwind();
        _ctx.builder->CreateRetVoid();
        return;
    }

    node.expr->accept(*_ctx.visitor);

    // check if we actually got a value on the stack
    if (_ctx.value_stack.empty()) {
        emit_unwind();
        _ctx.builder->CreateRetVoid();
        return;
    }

    llvm::Value *ret = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // a return fits its value to the declared return type through the same conversion table a
    // declaration, an assignment and a member write use - signedness lives on the ValueType and
    // not on the lowered llvm type, so `i8 -> i32` is a sign extend for int8 and a zero extend
    // for uint8. without it `return 0` in a `: float64` function reached CreateRet as an i32,
    // because a literal is typed where it is written and nothing there knows the return type
    //
    // result_type() may answer void (a binary expression whose operands differ does), which
    // coerce_value takes as "read the signedness off the value itself". a file-scope return has
    // no signature to answer to, and a value handed back from a `void` function is a semantic
    // error for the checker rather than a conversion to invent here
    if (_ctx.current_function != nullptr && !_ctx.current_function->get_return_type().is_void()) {
        ret = _ctx.types->coerce_value(
            ret, node.expr->result_type(), _ctx.current_function->get_return_type(),
            *_ctx.current_cmp_unit);
    }

    // after the value is computed and coerced, before the ret. this ordering is the point of
    // ReturnNode::unwind: `return $c->x` over an owning `$c` reads the block and then gives it back
    emit_unwind();

    _ctx.builder->CreateRet(ret);
}

void StmtCodegen::gen_guard(AST::GuardNode &node)
{
    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    auto *bound_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "guard.bound", function);
    auto *else_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "guard.else", function);

    // the slot before the branch, like every other local: ensure_var_slot allocates in the function's
    // entry block, so where the declaration sits among its siblings decides nothing
    llvm::AllocaInst *slot = ensure_var_slot(*node.decl);

    // **evaluated exactly once.** the same value is tested and then stored, which is what makes
    // `guard $n = $cache->lookup($k) else {...}` call `lookup` once rather than once per path
    node.decl->init_expr->accept(*_ctx.visitor);
    llvm::Value *optional = _ctx.pop();

    const AST::ValueType optional_type = node.decl->init_expr->result_type();

    _ctx.builder->CreateCondBr(
        _ctx.types->gen_has_value(optional, optional_type), bound_block, else_block);

    // the bound path: unwrap and store. `coerce_value` is still asked, because the declared type may be a
    // widening of the payload - `guard int64 $v = lookup($k)` over an `int32?`
    _ctx.builder->SetInsertPoint(bound_block);

    llvm::Value *value = _ctx.types->gen_unwrapped(optional, optional_type);
    _ctx.builder->CreateStore(
        _ctx.types->coerce_value(
            value,
            AST::unwrapped_type_of(optional_type),
            node.decl->type(),
            *_ctx.current_cmp_unit),
        slot);

    // and control falls out of the *bound* block into whatever follows the guard, which is the whole
    // shape of the statement: the binding is in scope from here, unconditionally
    auto *continue_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "guard.continue", function);
    _ctx.builder->CreateBr(continue_block);

    // the absent path. it never rejoins - AST::scope_always_exits refused an else arm that could fall
    // through - but the branch is emitted defensively rather than assumed: an unterminated block is an
    // llvm verifier failure with no source location, and this is cheap insurance against one
    _ctx.builder->SetInsertPoint(else_block);
    node.else_scope->accept(*_ctx.visitor);

    if (!_ctx.block_is_terminated()) {
        _ctx.builder->CreateBr(continue_block);
    }

    _ctx.builder->SetInsertPoint(continue_block);
}

void StmtCodegen::gen_if_statement(AST::IfStatementNode &node)
{
    llvm::BasicBlock *if_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "if", _ctx.builder->GetInsertBlock()->getParent());
    llvm::BasicBlock *merge_block = nullptr;

    // condition
    node.condition->accept(*_ctx.visitor);
    llvm::Value *condition = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // if there is no else block we directly jump to the merge block
    if (!node.else_scope) {
        merge_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "merge", _ctx.builder->GetInsertBlock()->getParent());
        _ctx.builder->CreateCondBr(condition, if_block, merge_block);

        // if block
        _ctx.builder->SetInsertPoint(if_block);
        node.if_scope->accept(*_ctx.visitor);

        // if last instruction is not a terminator we need to add a branch to the merge block
        if (!_ctx.block_is_terminated()) {
            _ctx.builder->CreateBr(merge_block);
        }

        // _ctx.builder->CreateBr(merge_block);
    } else {
        llvm::BasicBlock *else_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "else", _ctx.builder->GetInsertBlock()->getParent());

        _ctx.builder->CreateCondBr(condition, if_block, else_block);

        // if block
        _ctx.builder->SetInsertPoint(if_block);
        node.if_scope->accept(*_ctx.visitor);
        // _ctx.builder->CreateBr(merge_block);

        if (!_ctx.block_is_terminated()) {
            merge_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "merge", _ctx.builder->GetInsertBlock()->getParent());
            _ctx.builder->CreateBr(merge_block);
        }

        // else block
        _ctx.builder->SetInsertPoint(else_block);
        node.else_scope->accept(*_ctx.visitor);
        // _ctx.builder->CreateBr(merge_block);

        if (!_ctx.block_is_terminated()) {
            if (!merge_block) {
                merge_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "merge", _ctx.builder->GetInsertBlock()->getParent());
            }
            _ctx.builder->CreateBr(merge_block);
        }
    }

    if (merge_block) {
        _ctx.builder->SetInsertPoint(merge_block);
    }
}

void StmtCodegen::gen_while_statement(AST::WhileStatementNode &node)
{
    llvm::BasicBlock *loop_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "loop", _ctx.builder->GetInsertBlock()->getParent());
    llvm::BasicBlock *body_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "body", _ctx.builder->GetInsertBlock()->getParent());
    llvm::BasicBlock *merge_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "merge", _ctx.builder->GetInsertBlock()->getParent());

    _ctx.builder->CreateBr(loop_block);

    // loop block
    _ctx.builder->SetInsertPoint(loop_block);
    node.condition->accept(*_ctx.visitor);
    llvm::Value *condition = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    _ctx.builder->CreateCondBr(condition, body_block, merge_block);

    // body block. the back edge is only emitted when the body can actually fall out of its end -
    // a body whose every path returns already terminated its block, and a second terminator there
    // fails the verifier
    _ctx.builder->SetInsertPoint(body_block);
    {
        // **the body only**, and the continue target is the *condition* block: for a `while` the
        // condition is the step, so `continue` and the natural back edge below are the same edge. that is
        // also why no step block is needed for the `foreach` this lowers from - its advance lives in the
        // condition by design
        LoopTargetScope loop(_ctx, merge_block, loop_block);

        node.loop_scope->accept(*_ctx.visitor);
    }
    if (!_ctx.block_is_terminated()) {
        _ctx.builder->CreateBr(loop_block);
    }

    // merge block
    _ctx.builder->SetInsertPoint(merge_block);
}

void StmtCodegen::gen_loop_control(AST::LoopControlNode &node)
{
    if (_ctx.loop_targets.empty()) {
        // Parser::parse_loop_control refuses one outside a loop and builds no node for it, so this is a
        // compiler bug rather than a program error - and the alternative is a branch to a null block,
        // which llvm reports nowhere near here
        throw _ctx.error(fmt::format("a '{}' reached codegen with no enclosing loop {}",
            node.keyword(), _ctx.function_context()));
    }

    const CodegenContext::LoopTarget &target = _ctx.loop_targets.back();

    // **the drops first, then the branch.** the same ordering gen_return uses before CreateRet, and for
    // the same reason: a terminator ends the block, so anything emitted after it is either dropped on the
    // floor or a second terminator the verifier rejects
    //
    // these are void calls and releases, so none of them pushes onto the value stack - which is what lets
    // gen_scope's per-statement depth assertion hold across an exit
    for (auto &drop : node.unwind) {
        if (drop.has()) {
            drop.node()->accept(*_ctx.visitor);
        }
    }

    _ctx.builder->CreateBr(
        node.kind == AST::LoopControlKind::t_break ? target.break_block : target.continue_block);
}

void StmtCodegen::gen_assign(AST::AssignNode &node)
{
    // **the right-hand side first, always.** it may read the very value this assignment is about to
    // tear down - `$a = $a` through a copy constructor, `$a = replace($a)` through a borrow - so
    // nothing below may be hoisted above this line. it is also where a class's retain sits, as a node
    // inside value_expr, which is what makes the release further down safe
    node.value_expr->accept(*_ctx.visitor);

    llvm::Value *new_value = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // one path for every left hand side shape, addressing exactly what the target names - and
    // addressing it once, which is why a class's release is not a node with a place of its own
    // write-through is not decided here: `$p = 20` arrives as a deref of $p and lands on the
    // pointee, `$p:$ = &$b` arrives as $p itself and lands on the slot, re-seating it
    auto place = _ctx.lvalues->gen_lvalue(*node.target);

    // the reference being overwritten, read before the store destroys it and released after it. see
    // AssignNode::teardown_old for why a class's teardown is a bool here and a struct's is a tree
    llvm::Value *old_handle = nullptr;
    if (node.releases_old) {
        old_handle = _ctx.lvalues->gen_load(place, "old");
    }

    // an owning struct is destroyed *in place*, so its teardown sits between the right-hand side and
    // the store that overwrites those bytes. ordinary void destructor calls, exactly as at a scope end
    if (node.teardown_old != nullptr) {
        node.teardown_old->accept(*_ctx.visitor);
    }

    _ctx.builder->CreateStore(
        _ctx.types->coerce_value(new_value, node.value_expr->result_type(), place.storage_type, *_ctx.current_cmp_unit),
        place.address);

    if (old_handle != nullptr) {
        _ctx.classes->gen_release_value(old_handle, place.storage_type);
    }
}
};
