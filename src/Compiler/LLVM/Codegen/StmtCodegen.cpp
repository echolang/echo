#include "Compiler/LLVM/Codegen/StmtCodegen.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/Codegen/ClassCodegen.h"
#include "Compiler/LLVM/Codegen/DebugInfoCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ASTFunctionEmission.h"
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
#include "AST/ForStatementNode.h"
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

    // the block's own debug scope, opened before the slots below because that is where their variables
    // are declared - two locals of one name in sibling blocks resolve apart only if each is recorded
    // under the block it was written in. a no-op for a scope nobody wrote a brace for, which is what
    // the answer here records for the matching pop
    const bool debug_block = _ctx.debug_info->push_lexical_block(node);

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

        // **the statement seam.** set once here and inherited by every load, call and branch the
        // subtree below emits, which is the whole of how a line table falls out of a walk that knows
        // nothing about lines. Per statement rather than per expression deliberately: that is the
        // granularity a `step` command means, and a location per expression node triples the metadata
        // for no debugger benefit
        _ctx.debug_info->set_location(*child.node());

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

    // the `break` above falls through to here, so this is the one exit and the stack stays balanced.
    // the early return at the top of this function is *ahead* of the push, deliberately
    _ctx.debug_info->pop_lexical_block(debug_block);
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

    // **once per slot per function**, which the guard above is already what guarantees: a second
    // declare record for one variable is what a re-entry here would produce. The record goes at the
    // alloca rather than at the written position, so it dominates every use - see declare_local
    _ctx.debug_info->declare_local(alloca, node, std::nullopt);

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
    // nothing to emit unless this compiler owns the body, which AST::function_emission_kind is the one
    // owner of. A template has no concrete signature; a builtin is answered at each call site and an
    // interface requirement dispatches through a vtable, so neither has a symbol at all; an extern and an
    // intrinsic have one somebody else supplies
    const AST::FunctionEmission emission = AST::function_emission_kind(&node);

    if (!AST::emission_has_body(emission)) {
        return;
    }

    // and a kind that claims a body and then has none is a compiler bug rather than a source error - the
    // source case is `AST::declaration_owes_a_body`'s, reported located by the type checker, which gates
    // compilation long before this: the
    // symbol was declared on the strength of the same answer, so returning quietly here would link
    // against something nobody defines. That is exactly what the bare `if (!is_generic()) return;` this
    // replaces used to swallow - a member whose body error recovery skipped past reached codegen, got a
    // `declare`, and produced an undefined symbol with nothing pointing at the declaration
    if (!node.body) {
        throw _ctx.error(fmt::format(
            "Function '{}' has no body associated with it.",
            node.func_name()
        ));
    }

    // track the enclosing function so codegen errors can name it. restored before returning
    AST::FunctionDeclNode *prev_function = _ctx.current_function;
    _ctx.current_function = &node;

    // and the file *this declaration* was written in, rather than whichever one the walk happens to be
    // standing in. The body can bake the file name into a constant - an `assert` message carries
    // `<file>:<line>` - so leaving it ambient makes the emitted bytes depend on the walk, which is not
    // allowed for a definition two units may both emit. See CodegenContext::function_file_map
    AST::File *prev_file = _ctx.current_file;
    _ctx.current_file = _ctx.file_of(&node);

    // dump all function names in map
    auto funcid = _ctx.current_cmp_unit->function_table.get_function_id_by_name(AST::mangle_function_name(&node));
    auto func = _ctx.current_cmp_unit->function_table.get_llvm_function(funcid);

    // the symbol has to have been declared into this unit before a body can be attached to it. Reported
    // rather than dereferenced: a null here used to crash inside BasicBlock::Create with nothing naming
    // the declaration, and the two ways to get one are both compiler bugs - build_function_maps skipped a
    // declaration it should have claimed, or a body is being emitted into a unit that never referenced it
    if (func == nullptr) {
        throw _ctx.error(fmt::format(
            "'{}' has no symbol in module '{}', so its body cannot be emitted there.",
            node.func_name(), _ctx.current_cmp_unit->ast_module->name));
    }

    // already emitted into this unit. An ODR-shared body is reachable twice - once from the drain's queue
    // and once from whatever named it - and BasicBlock::Create below is unconditional, so a second pass
    // would give one llvm::Function two entry blocks
    if (!func->empty()) {
        _ctx.current_function = prev_function;
        _ctx.current_file = prev_file;
        return;
    }

    // a generated definition claims its symbol weakly, because more than one unit may legitimately hold
    // the same one - see AST::FunctionEmission::t_odr_shared. The flip happens here rather than at
    // Function::Create because LLVM's verifier rejects a *bodyless* linkonce_odr function: a unit that
    // only references the symbol must declare it externally, and only the unit that supplies the body
    // may weaken it
    if (emission == AST::FunctionEmission::t_odr_shared) {
        func->setLinkage(llvm::Function::LinkOnceODRLinkage);
    }

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", func);
    _ctx.set_insert_point(entry);

    // the subprogram, once the body is certain to be emitted here - a declaration may carry no !dbg,
    // so this cannot move up to Function::Create. Same place and same reason as the linkage flip above
    _ctx.debug_info->begin_function(node, func);

    // **the prologue sits at the subprogram's own line**, which is what makes LLVM place `prologue_end`
    // at the first statement of the body rather than on the declaration. The allocas themselves stay
    // unlocated: entry_alloca builds with its own IRBuilder, which starts with no location
    _ctx.debug_info->set_function_scope_location();

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

        // 1-based, which is the whole of what distinguishes a parameter from a local in DWARF
        _ctx.debug_info->declare_local(alloca, *node.args[arg.getArgNo()], arg.getArgNo() + 1);
    }

    // a synthesized constructor arrives here like any other function - the struct parser builds its
    // body out of the same nodes a user would write, which is what keeps one implementation of the
    // member write and of the pointer re-seat
    node.body->accept(*_ctx.visitor);

    // the synthesized terminator belongs to the function rather than to any statement, so it takes the
    // subprogram's line - left where the last statement stood, an epilogue reports a line control
    // already left
    _ctx.debug_info->set_function_scope_location();

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

    _ctx.debug_info->end_function();

    _ctx.current_function = prev_function;
    _ctx.current_file = prev_file;
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

    // **exactly one of `init_expr` and `presence_test` is the value evaluated before the branch**, and it
    // is evaluated exactly once. that is GuardNode's stated invariant, and the branch below is the whole
    // of what this function knows about which protocol answered
    llvm::Value *condition = nullptr;

    // the tested optional, on the builtin path only. the protocol path leaves both unset and always
    // carries a `bound_value`, so the unwrap arm below never reads them
    llvm::Value *optional = nullptr;
    AST::ValueType optional_type;

    if (node.presence_test != nullptr) {
        // **the type answered the presence question**, through `contract::unwrappable<V>::has_value()`.
        // there is no optional here to unwrap: AST::GuardLowering hoisted the subject into an ordinary
        // declaration ahead of this statement, so what the binding is given hangs off `bound_value`
        // below - which is the path a tagged optional read out of a place has always taken
        node.presence_test->accept(*_ctx.visitor);

        condition = _ctx.types->coerce_value(
            _ctx.pop(),
            node.presence_test->result_type(),
            AST::ValueType(AST::ValueTypePrimitive::t_bool),
            *_ctx.current_cmp_unit);
    }
    else {
        // **evaluated exactly once.** the same value is tested and then stored, which is what makes
        // `$n = guard $cache->lookup($k) else {...}` call `lookup` once rather than once per path
        //
        // the `bound_value` path below stores a *copy* read back out of this same place instead. that
        // re-reads the place, it does not re-evaluate a call: AST::OwnershipPass mints that path only for
        // a place, and a call's result is a wrapper nobody owns whose payload it moves out of here
        node.decl->init_expr->accept(*_ctx.visitor);
        optional = _ctx.pop();
        optional_type = node.decl->init_expr->result_type();

        condition = _ctx.types->gen_has_value(optional, optional_type);
    }

    _ctx.builder->CreateCondBr(condition, bound_block, else_block);

    // the bound path: unwrap and store. `coerce_value` is still asked, because the declared type may be a
    // widening of the payload - `guard int64 $v = lookup($k)` over an `int32?`
    _ctx.set_insert_point(bound_block);

    llvm::Value *bound = nullptr;
    AST::ValueType bound_type;

    if (node.bound_value != nullptr) {
        // **the payload was copied rather than moved out**, because the tested value is a place somebody
        // else still owns. AST::OwnershipPass built the copy over the `__value` place and hung it here
        node.bound_value->accept(*_ctx.visitor);

        bound = _ctx.pop();
        bound_type = node.bound_value->result_type();
    }
    else {
        bound = _ctx.types->gen_unwrapped(optional, optional_type);
        bound_type = AST::unwrapped_type_of(optional_type);
    }

    // one store for both, because only the value and the type it comes from differ: what a guard's binding
    // is *given* is the arm's question, how it is seated is the statement's
    _ctx.builder->CreateStore(
        _ctx.types->coerce_value(bound, bound_type, node.decl->type(), *_ctx.current_cmp_unit),
        slot);

    // and control falls out of the *bound* block into whatever follows the guard, which is the whole
    // shape of the statement: the binding is in scope from here, unconditionally
    auto *continue_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "guard.continue", function);
    _ctx.builder->CreateBr(continue_block);

    // the absent path. it never rejoins - AST::scope_always_exits refused an else arm that could fall
    // through - but the branch is emitted defensively rather than assumed: an unterminated block is an
    // llvm verifier failure with no source location, and this is cheap insurance against one
    _ctx.set_insert_point(else_block);
    node.else_scope->accept(*_ctx.visitor);

    if (!_ctx.block_is_terminated()) {
        _ctx.builder->CreateBr(continue_block);
    }

    _ctx.set_insert_point(continue_block);
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
        _ctx.set_insert_point(if_block);
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
        _ctx.set_insert_point(if_block);
        node.if_scope->accept(*_ctx.visitor);
        // _ctx.builder->CreateBr(merge_block);

        if (!_ctx.block_is_terminated()) {
            merge_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "merge", _ctx.builder->GetInsertBlock()->getParent());
            _ctx.builder->CreateBr(merge_block);
        }

        // else block
        _ctx.set_insert_point(else_block);
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
        _ctx.set_insert_point(merge_block);
    }
}

void StmtCodegen::gen_while_statement(AST::WhileStatementNode &node)
{
    // no step: for a `while` the condition *is* the step, which is also why the `foreach` this lowers
    // from needs no step block - its advance lives in the condition by design
    gen_loop(*node.condition, nullptr, *node.loop_scope);
}

void StmtCodegen::gen_for_statement(AST::ForStatementNode &node)
{
    gen_loop(*node.condition, node.step, *node.loop_scope);
}

void StmtCodegen::gen_loop(AST::ExprNode &condition, AST::ScopeNode *step, AST::ScopeNode &body)
{
    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    llvm::BasicBlock *loop_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "loop", function);
    llvm::BasicBlock *body_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "body", function);
    llvm::BasicBlock *step_block = step != nullptr
        ? llvm::BasicBlock::Create(*_ctx.llvm_context, "step", function)
        : nullptr;
    llvm::BasicBlock *merge_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "merge", function);

    // where the body falls out to and where a `continue` goes - the same block, always, which is what
    // makes `continue` and the natural back edge indistinguishable to everything below
    llvm::BasicBlock *next_block = step_block != nullptr ? step_block : loop_block;

    _ctx.builder->CreateBr(loop_block);

    // loop block
    _ctx.set_insert_point(loop_block);
    condition.accept(*_ctx.visitor);
    llvm::Value *condition_value = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    _ctx.builder->CreateCondBr(condition_value, body_block, merge_block);

    // body block. the back edge is only emitted when the body can actually fall out of its end -
    // a body whose every path returns already terminated its block, and a second terminator there
    // fails the verifier
    _ctx.set_insert_point(body_block);
    {
        // **the body only.** the condition above and the step below are outside the guard: a `break`
        // written in either belongs to an enclosing loop, and neither is a place a loop exit can be
        // written anyway
        LoopTargetScope loop(_ctx, merge_block, next_block);

        body.accept(*_ctx.visitor);
    }
    if (!_ctx.block_is_terminated()) {
        _ctx.builder->CreateBr(next_block);
    }

    // step block, and the back edge out of it. terminated even where nothing can reach it - a body whose
    // every path returns and holds no `continue` leaves this block unreachable, and an unreachable block
    // still owes the verifier a terminator. guarded exactly as the body's edge is, and for the same
    // reason rather than for a case a step can reach today: the guard says "unless the step already
    // ended it", which is what keeps the answer the step's own rather than this function's
    if (step_block != nullptr) {
        _ctx.set_insert_point(step_block);
        step->accept(*_ctx.visitor);

        if (!_ctx.block_is_terminated()) {
            _ctx.builder->CreateBr(loop_block);
        }
    }

    // merge block
    _ctx.set_insert_point(merge_block);
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

    // through the seam and not a bare CreateStore: the place carries the provenance that decides
    // whether this write may be tagged, and it is the one store in the compiler that a user program's
    // `=` reaches
    _ctx.lvalues->gen_store(
        place,
        _ctx.types->coerce_value(new_value, node.value_expr->result_type(), place.storage_type, *_ctx.current_cmp_unit));

    if (old_handle != nullptr) {
        _ctx.classes->gen_release_value(old_handle, place.storage_type);
    }
}
};
