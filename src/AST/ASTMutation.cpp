#include "AST/ASTMutation.h"

#include "AST/ASTAccess.h"
#include "AST/ASTArgumentBind.h"
#include "AST/ASTCallResolution.h"
#include "AST/ASTCollector.h"
#include "AST/ASTCopy.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/ForeachNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ReturnNode.h"
#include "AST/VarDeclNode.h"

#include <unordered_set>

namespace
{
    // how a bound slot sits relative to eliding `$el` to `const V&`. a write is a write; t_other is
    // a promotion (or anything that is not V) that would miss after elision; the two `*_same` cases
    // are the mixed-set test
    enum class ElisionSlot
    {
        t_write,
        t_read_same,
        t_by_value_same,
        t_other,
    };

    // the slot consumes this argument's type, not a promotion of it. `str::from($el)` of a
    // `usize` ranks `from(uint64)` as a promotion; eliding to `const usize&` makes every
    // candidate miss, because read-through wants an exact pointee and promotion wants a
    // primitive. asked of the argument, so `$el?->trace()` compares `Item` (the marker) with
    // the receiver, not the `Item?` binding
    bool parameter_reads_argument_as_itself(
        const AST::FunctionDeclNode &fn,
        size_t index,
        AST::ExprNode *arg
    )
    {
        if (arg == nullptr || index >= fn.args.size() || fn.args[index] == nullptr
            || !fn.args[index]->has_type()) {
            return false;
        }

        AST::ValueType wanted = fn.args[index]->type();
        AST::ValueType given = arg->result_type();

        if (wanted.is_pointer()) {
            wanted = wanted.pointee();
        }

        if (given.is_pointer()) {
            given = given.pointee();
        }

        if (AST::is_undetermined_type(wanted) || AST::is_undetermined_type(given)) {
            return false;
        }

        return AST::ValueType::make_mutable(wanted) == AST::ValueType::make_mutable(given);
    }

    ElisionSlot elision_slot_of(
        const AST::FunctionDeclNode &fn,
        size_t index,
        const AST::ValueType &source,
        AST::ExprNode *arg
    )
    {
        if (index >= fn.args.size() || fn.args[index] == nullptr) {
            return ElisionSlot::t_write;
        }

        const AST::AccessEffect effect = AST::access_effect_of(fn, index);

        if (AST::access_is_exclusive(effect)) {
            return ElisionSlot::t_write;
        }

        const bool same = parameter_reads_argument_as_itself(fn, index, arg);

        // inferred for every `const T&`. a read cannot write the place; a promotion still
        // prevents elision
        if (effect == AST::AccessEffect::t_read) {
            return same ? ElisionSlot::t_read_same : ElisionSlot::t_other;
        }

        if (!fn.args[index]->has_type()) {
            return ElisionSlot::t_write;
        }

        const AST::ValueType param = fn.args[index]->type();

        // `T&` / `ptr<T>`: the callee can write through it. `const T&` already returned above
        if (param.is_pointer()) {
            return ElisionSlot::t_write;
        }

        // by-value: a copy at the call. eliding the loop binding makes the source const, so a copy
        // ctor that needs a mutable source, or a type with no copy at all, would fail there
        if (source.is_unknown() || AST::classify_copy(source) == AST::CopyKind::t_none
            || !AST::copy_source_may_be_const(source)) {
            return ElisionSlot::t_write;
        }

        return same ? ElisionSlot::t_by_value_same : ElisionSlot::t_other;
    }

    class WriteFinder : public AST::RecursiveVisitor
    {
    public:
        WriteFinder(const AST::VarDeclNode &decl, AST::Collector &collector)
            : _decl(&decl), _collector(&collector)
        {}

        bool found() const { return _written; }

        void visit_assign(AST::AssignNode &node) override
        {
            // the target's *root*, so `$el = v`, `$el->x = v`, `$el[$i] = v` and `$el:$ = v` are one
            // question - AST::place_root_of already walks all four shapes
            note(node.target);
            AST::RecursiveVisitor::visit_assign(node);
        }

        void visit_addr_of_expr(AST::AddrOfExprNode &node) override
        {
            // a receiver AddrOf is visitFunctionCallExpr's question. a bare `&$el` still writes:
            // `$p = &$el` and `look(&$el)` would dangle the moment the iteration steps
            if (_parser_receivers.count(&node) == 0) {
                note(node.operand);
            }

            AST::RecursiveVisitor::visit_addr_of_expr(node);
        }

        void visit_move_expr(AST::MoveExprNode &node) override
        {
            note(node.operand);
            AST::RecursiveVisitor::visit_move_expr(node);
        }

        void visitFunctionCallExpr(AST::FunctionCallExprNode &node) override
        {
            // `echo` is the exception, through the one owner of "is this the decl-less print builtin".
            // it neither writes nor borrows
            if (!AST::is_print_call(node)) {
                // skip the implicit receiver wrap so visit_addr_of_expr does not count `$el->m()`
                // or `$el?->m()` as `&$el`. the call still answers, through place_is_decl
                if (AST::AddrOfExprNode *addrof = AST::parser_receiver_addrof(node, *_decl)) {
                    _parser_receivers.insert(addrof);
                } else if (node.is_member_lookup()
                    && node.arguments[0]->get_node_type() == AST::NodeType::n_expr_addrof
                    && is_active_chain_base(node.arguments[0])) {
                    _parser_receivers.insert(
                        static_cast<AST::AddrOfExprNode *>(node.arguments[0]));
                }

                std::vector<AST::FunctionDeclNode *> candidates;
                bool fetched = false;

                for (AST::ExprNode *arg : node.arguments) {
                    if (!place_is_decl(arg)) {
                        continue;
                    }

                    if (!fetched) {
                        candidates = candidates_of(node);
                        fetched = true;
                    }

                    if (prevents_elision(candidates, node, arg)) {
                        _written = true;
                    }
                }
            }

            AST::RecursiveVisitor::visitFunctionCallExpr(node);
        }

        void visit_optional_chain(AST::OptionalChainExprNode &node) override
        {
            // identity only. nested `?->` through this decl has to stay in the set so
            // `$el?->items[0]->bump()` is place_is_decl
            const bool through =
                AST::place_root_of(node.base) == _decl || is_active_chain_base(node.base);

            if (through && node.chain_base != nullptr) {
                _chain_bases.insert(node.chain_base);
            }

            AST::RecursiveVisitor::visit_optional_chain(node);

            if (through && node.chain_base != nullptr) {
                _chain_bases.erase(node.chain_base);
            }
        }

        void visit_indirect_call_expr(AST::IndirectCallExprNode &node) override
        {
            for (auto *arg : node.arguments) {
                note(arg);
            }

            AST::RecursiveVisitor::visit_indirect_call_expr(node);
        }

        void visitReturn(AST::ReturnNode &node) override
        {
            // returning it reads it *out*, which from a `const V&` is a copy - and if V owns something
            // with no copy constructor that is a diagnostic the value binding would not have earned
            note(node.expr);
            AST::RecursiveVisitor::visitReturn(node);
        }

        void visit_closure_expr(AST::ClosureExprNode &node) override
        {
            // a captured `const V&` dangles the moment the iteration steps
            for (auto *value : node.captured_values) {
                note(value);
            }

            AST::RecursiveVisitor::visit_closure_expr(node);
        }

        void visit_index_expr(AST::IndexExprNode &node) override
        {
            // a bracket whose base has already moved into its element_call cannot be seen through, so
            // it is answered *written* rather than guessed at. OperatorRewriter already ran this
            // round, so `$el[$k]` is still an IndexExprNode - and the conservative answer if it is not
            if (node.base == nullptr) {
                _written = true;
            }

            AST::RecursiveVisitor::visit_index_expr(node);
        }

        void visit_foreach(AST::ForeachNode &node) override
        {
            // a nested loop over it takes its address, exactly as case (a) of the lowering does
            note(node.source);
            AST::RecursiveVisitor::visit_foreach(node);
        }

    private:
        bool is_active_chain_base(AST::ExprNode *expr) const
        {
            const AST::ChainBaseNode *base = AST::chain_base_of(expr);

            return base != nullptr && _chain_bases.count(base) != 0;
        }

        bool optional_chain_of_decl(AST::ExprNode *expr) const
        {
            for (; expr != nullptr && expr->get_node_type() == AST::NodeType::n_expr_optional_chain;
                 expr = static_cast<AST::OptionalChainExprNode *>(expr)->continuation)
            {
                if (AST::place_root_of(static_cast<AST::OptionalChainExprNode *>(expr)->base) == _decl) {
                    return true;
                }
            }

            return false;
        }

        bool place_is_decl(AST::ExprNode *expr) const
        {
            return AST::place_root_of(expr) == _decl
                || optional_chain_of_decl(expr)
                || is_active_chain_base(expr);
        }

        std::vector<AST::FunctionDeclNode *> candidates_of(AST::FunctionCallExprNode &call)
        {
            if (call.decl != nullptr) {
                return {call.decl};
            }

            return AST::CallResolver(*_collector).candidates_for(call);
        }

        void note(AST::ExprNode *expr)
        {
            if (place_is_decl(expr)) {
                _written = true;
            }
        }

        AST::ValueType source_type() const
        {
            if (!_decl->has_type()) {
                return AST::ValueType::make_unknown();
            }

            return AST::ValueType::make_mutable(_decl->type());
        }

        bool prevents_elision(
            const std::vector<AST::FunctionDeclNode *> &candidates,
            AST::FunctionCallExprNode &call,
            AST::ExprNode *arg
        )
        {
            if (candidates.empty()) {
                return true;
            }

            const AST::ValueType source = source_type();
            bool any_bound = false;
            bool any_read = false;
            bool any_by_value = false;

            for (AST::FunctionDeclNode *candidate : candidates) {
                if (candidate == nullptr) {
                    continue;
                }

                const AST::ArgumentBinding binding =
                    AST::bind_arguments(*candidate, call.arguments, call.argument_names);

                if (binding.kind != AST::ArgumentBindKind::t_ok) {
                    continue;
                }

                size_t slot = binding.filled.size();

                for (size_t i = 0; i < binding.filled.size(); i++) {
                    if (!binding.filled[i].from_default && binding.filled[i].value == arg) {
                        slot = i;
                        break;
                    }
                }

                if (slot == binding.filled.size()) {
                    continue;
                }

                any_bound = true;

                switch (elision_slot_of(*candidate, slot, source, arg)) {
                case ElisionSlot::t_write:
                case ElisionSlot::t_other:
                    return true;
                case ElisionSlot::t_read_same:
                    any_read = true;
                    break;
                case ElisionSlot::t_by_value_same:
                    any_by_value = true;
                    break;
                }
            }

            // no candidate bound this list: unknown, or still waiting on a type
            if (!any_bound) {
                return true;
            }

            // by-value `$el` ranks `f(T)` exact over `f(const T&)`. eliding makes the borrow exact
            return any_by_value && any_read;
        }

        const AST::VarDeclNode *_decl = nullptr;
        AST::Collector *_collector = nullptr;
        std::unordered_set<const AST::AddrOfExprNode *> _parser_receivers;
        std::unordered_set<const AST::ChainBaseNode *> _chain_bases;
        bool _written = false;
    };
}

bool AST::is_never_written(const AST::VarDeclNode &decl, AST::Node &subtree, AST::Collector &collector)
{
    WriteFinder finder(decl, collector);
    subtree.accept(finder);

    return !finder.found();
}
