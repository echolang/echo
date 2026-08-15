#include "AST/ASTAccessPass.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

namespace AST
{

AccessPass::AccessPass(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

void AccessPass::run()
{
    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();
        for (auto &file : module_ptr->files()) {
            _current_file = &file;
            if (file.root) {
                file.root->accept(*this);
            }
        }
    }
}

void AccessPass::visitFunctionCallExpr(FunctionCallExprNode &node)
{
    // the descent first: a conflict inside an argument is still a conflict, and reporting the outer
    // call before the inner one would order the diagnostics by nesting rather than by position
    RecursiveVisitor::visitFunctionCallExpr(node);

    check_call(node);
}

void AccessPass::visitFunctionDecl(FunctionDeclNode &node)
{
    FunctionDeclNode *previous = _current_function;
    _current_function = &node;

    RecursiveVisitor::visitFunctionDecl(node);

    _current_function = previous;
}

const VarDeclNode *AccessPass::read_parameter_reached_by(ExprNode *expr) const
{
    if (_current_function == nullptr || expr == nullptr) {
        return nullptr;
    }

    // the root and not the path: nothing below reads a projection, and building one costs a vector, a
    // string per field step and an AST::const_fold per indexed one - per argument of every resolved
    // call in the program, and per assignment target
    const VarDeclNode *root = access_root_of(expr);
    if (root == nullptr) {
        return nullptr;
    }

    // a parameter of *this* function, and only a declared `read` - never one inferred from `const`.
    //
    // the split matters: `const` is a per-level flag, so a `const array<T>&` already permits writing
    // through a member pointer and always has. widening this to every const borrow would refuse
    // programs that were legal before, which is the one thing the rule may not do - so `read` is what
    // an author writes when they want the promise checked, and `const` keeps meaning exactly what it
    // meant
    for (const VarDeclNode *param : _current_function->args) {
        if (param != root || declared_access_effect(*param) != AccessEffect::t_read) {
            continue;
        }

        // **a borrow, and deliberately not a by-value parameter.**
        //
        // for a `read array<T>& $src` the region is the caller's storage, which is what the promise
        // is about and what an emitted `readonly` claims. for a `read slice<T> $src` it is not: the
        // descriptor is a *copy*, and the region the author means is the window it views - which the
        // compiler cannot yet follow back to its owner. enforcing against the copy instead would
        // refuse `$src[$i]`, an ordinary read, because the element operator takes its receiver by
        // mutable borrow.
        //
        // so a by-value view declares intent and is not held to it yet. that is the slice-provenance
        // half of the work, and until it lands nothing is lowered from one either
        if (!param->has_type() || !param->type().is_pointer()) {
            continue;
        }

        return param;
    }

    return nullptr;
}

void AccessPass::visit_assign(AssignNode &node)
{
    RecursiveVisitor::visit_assign(node);

    const VarDeclNode *param = read_parameter_reached_by(node.target);
    if (param == nullptr) {
        return;
    }

    _collector.collect_issue<Issue::GenericError>(
        CodeRef{_current_module, node.token_assign.make_slice()},
        fmt::format(
            "'{}' takes 'read' access, so this writes to storage the parameter promised only to read. "
            "Declare it 'inout' if the body is meant to write through it.",
            param->name_full()));
}

void AccessPass::check_call(FunctionCallExprNode &node)
{
    // a call nothing resolved has no parameters to read effects off. legitimate rather than a bug -
    // a refused call still reaches here, and it has already been reported
    if (node.decl == nullptr) {
        return;
    }

    const size_t count = std::min(node.arguments.size(), node.decl->args.size());

    // **a read region may not escape into a position that could write it.**
    //
    // the second half of what makes `read` a promise rather than a comment. the assignment arm above
    // catches the body writing through it directly; this catches the body handing the address to
    // something that will. without both, an emitted `readonly` - which says the function writes
    // nothing through the argument *or anything based on it* - is simply false
    //
    // ahead of the arity guard below, because a one-argument call has no *pair* to conflict and can
    // still hand a region away
    for (size_t i = 0; i < count; i++) {
        check_read_escape(node, i);
    }

    // a conflict needs two accesses
    if (count < 2) {
        return;
    }

    std::vector<AccessEffect> effects(count, AccessEffect::t_none);
    std::vector<AccessPath> paths(count);

    for (size_t i = 0; i < count; i++) {
        effects[i] = access_effect_of(*node.decl, i);

        // an effect nothing declared conflicts with nothing, so the path is not worth walking
        if (effects[i] != AccessEffect::t_none) {
            paths[i] = access_path_of(node.arguments[i]);
        }
    }

    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (!access_effects_conflict(effects[i], effects[j])) {
                continue;
            }

            // **`t_overlap` and not "anything but disjoint".** a path this cannot see through is
            // exactly the case where refusing would cost a working program for a suspicion
            if (path_overlap(paths[i], paths[j]) != Overlap::t_overlap) {
                continue;
            }

            report_conflict(node, i, j, effects[i], effects[j]);
        }
    }
}

void AccessPass::check_read_escape(FunctionCallExprNode &node, size_t index)
{
    // **the parameter first, the argument second.** these three are pointer and enum comparisons and
    // they reject nearly every argument in a program; the walk below is the expensive half, so asking
    // it first would pay for it on each of them
    const VarDeclNode *param = node.decl->args[index];
    if (param == nullptr || !param->has_type()) {
        return;
    }

    // an argument that is *copied* carries no address, so nothing the callee does can reach back.
    // only a pointer-shaped parameter - a borrow or a `ptr<T>` - lets the region escape
    if (!param->type().is_pointer()) {
        return;
    }

    const AccessEffect wanted = access_effect_of(*node.decl, index);
    if (wanted == AccessEffect::t_read) {
        return;
    }

    const VarDeclNode *source = read_parameter_reached_by(node.arguments[index]);
    if (source == nullptr) {
        return;
    }

    _collector.collect_issue<Issue::GenericError>(
        CodeRef{_current_module, location_of_expression(node.arguments[index]).make_slice()},
        fmt::format(
            "'{}' takes 'read' access, so its storage cannot be handed to '{}', which does not. "
            "Declare '{}' 'read' as well, or take '{}' as 'inout' if it is meant to be written.",
            source->name_full(),
            param->name_full(),
            param->name_full(),
            source->name_full()));
}

void AccessPass::report_conflict(
    FunctionCallExprNode &node,
    size_t first,
    size_t second,
    AccessEffect first_effect,
    AccessEffect second_effect
)
{
    // the exclusive one is named first: it is the access that cannot be shared, so it is the one the
    // reader has to move. when both are exclusive the order is the argument order, which is the order
    // they are written in.
    //
    // normalised by swapping rather than by selecting each value through the same condition - all of
    // these are by value, and one condition read six times is six places for the polarity to invert
    if (!access_is_exclusive(first_effect)) {
        std::swap(first, second);
        std::swap(first_effect, second_effect);
    }

    const size_t owner = first;
    const size_t other = second;
    const AccessEffect owner_effect = first_effect;
    const AccessEffect other_effect = second_effect;
    const VarDeclNode *owner_param = node.decl->args[owner];
    const VarDeclNode *other_param = node.decl->args[other];

    const std::string message = fmt::format(
        "This names the same storage as another argument of the same call. "
        "'{}' takes '{}' access, which is exclusive, so nothing else may reach that storage "
        "while the call runs - and '{}' takes '{}' access to it too",
        owner_param->name_full(),
        access_effect_spelling(owner_effect),
        other_param->name_full(),
        access_effect_spelling(other_effect));

    // **the remedy names a type, never an operation on it.** the useful advice for a container is
    // "ask it for the range instead of handing it itself", and the operation that does so is the
    // library's to name - a `#[core:]` name hardcoded here would be a message that stops being true
    // the moment the standard library renames anything
    std::string remedy =
        "a value cannot be both written and read by one call - read the source into a separate "
        "value first";

    if (owner == 0 && node.decl->has_receiver() && owner_param->has_type()) {
        remedy = fmt::format(
            "a value cannot be both written and read by one call. copy the source first, or use an "
            "operation of '{}' that names the part of the receiver to read instead of taking a "
            "second one",
            value_type_of(owner_param->type()).get_type_desciption());
    }

    // **anchored at the call and not at either argument.** the conflict is a property of the call -
    // neither argument is wrong on its own - and a receiver has no location of its own to point at
    // anyway: the parser synthesises it, so its varref carries the declaration's token and a
    // diagnostic pointed there lands on a line that has nothing to do with the problem
    _collector.collect_issue<Issue::ConflictingAccess>(
        CodeRef{_current_module, node.token_function_name.make_slice()},
        message,
        location_of_expression(node.arguments[other]),
        remedy);
}

};
