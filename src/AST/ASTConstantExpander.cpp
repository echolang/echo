#include "AST/ASTConstantExpander.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTConstness.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTSymbol.h"
#include "AST/ConstRefExprNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeDeclNode.h"

#include <fmt/core.h>

namespace AST
{

ConstDeclNode *find_constant(
    NamespaceManager &namespaces, const std::string &name, const Namespace *from, bool qualified)
{
    if (from == nullptr) {
        return nullptr;
    }

    Symbol *symbol = qualified
        ? namespaces.find_symbol(name, *from)
        : namespaces.find_symbol_in_scope(name, *from);

    if (symbol == nullptr) {
        return nullptr;
    }

    // a slot holds one node per name, so a hit that is a *type* is a name that is taken rather than a
    // constant. answered by asking the node, which is what every other reader of a symbol does
    return symbol->node.get_ptr<ConstDeclNode>();
}

ConstantExpander::ConstantExpander(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

CodeRef ConstantExpander::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, _current_file, token.make_slice()};
}

CodeRef ConstantExpander::code_ref_at_declaration(const ConstDeclNode &decl)
{
    // the declaring file, not the walked one: the excerpt is read out of the file's own content, and a cycle
    // is as likely to be noticed from another file as from the one that wrote it
    const File *file = decl.declared_in_file != nullptr ? decl.declared_in_file : _current_file;

    return CodeRef{_current_module, file, decl.token_name.make_slice()};
}

bool ConstantExpander::module_may_name(const ConstDeclNode &decl) const
{
    if (decl.declared_in_module == nullptr || _current_module == nullptr) {
        return true;
    }

    if (decl.declared_in_module == _current_module) {
        return true;
    }

    const auto declared_at = _module_order.find(decl.declared_in_module);
    const auto used_at = _module_order.find(_current_module);

    if (declared_at == _module_order.end() || used_at == _module_order.end()) {
        return true;
    }

    return declared_at->second < used_at->second;
}

void ConstantExpander::run()
{
    // parse order first, since resolution below is checked against it
    size_t index = 0;
    for (auto &module_ptr : _bundle.modules) {
        _module_order[module_ptr.get()] = index++;
    }

    // **the constants themselves first.** a use site then clones a tree that is already expanded, so one
    // chain of constants is walked once rather than once per reference - and the cycle guard has a single
    // place to live rather than being re-entered from every body
    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();
        _current_file = module_ptr->files().first();

        // by value: expanding one appends clones to this very collection, and of_type hands back a
        // reference to the vector those clones are pushed onto
        const std::vector<ConstDeclNode *> declarations = module_ptr->nodes.of_type<ConstDeclNode>();

        for (ConstDeclNode *decl : declarations) {
            expand_initializer(*decl);
        }
    }

    // then every body, through the one walk that reaches all of them: a function's body hangs off its
    // declaration and every declaration - method, constructor, destructor - is a root child, and
    // visit_type_decl descends into the properties, so a property initializer is covered by the same walk
    for (auto &module_ptr : _bundle.modules) {
        // **a module with no reference in it is skipped whole.** every ConstRefExprNode a body can hold was
        // emplaced into that body's own module by the parser, and the clones loop 1 appends carry none - so
        // an empty arena is proof there is nothing here to expand. This is what keeps the standard library,
        // which declares no constants at all, off the walk on every single compile
        if (module_ptr->nodes.of_type<ConstRefExprNode>().empty()) {
            continue;
        }

        _current_module = module_ptr.get();
        for (auto &file : module_ptr->files()) {
            _current_file = &file;
            if (file.root) {
                file.root->accept(*this);
            }
        }
    }
}

void ConstantExpander::expand_initializer(ConstDeclNode &decl)
{
    switch (decl.expansion) {
    case ConstExpansion::t_expanded:
    case ConstExpansion::t_refused:
        return;

    case ConstExpansion::t_expanding:
        // reached while already being expanded, which is a constant defined in terms of itself. Refused
        // rather than unfolded: substitution has no fixed point to stop at, so this is not a value that
        // could be computed with more effort
        _collector.collect_issue<Issue::GenericError>(
            code_ref_at_declaration(decl),
            fmt::format(
                "The constant '{}' is defined in terms of itself. Its value is copied to each use site, so "
                "a cycle would expand forever.",
                decl.name()));

        decl.expansion = ConstExpansion::t_refused;
        decl.value = &_current_module->nodes.emplace_back<VoidExprNode>();
        return;

    case ConstExpansion::t_pending:
        break;
    }

    decl.expansion = ConstExpansion::t_expanding;

    // `self::` inside a struct's constant means that struct
    ComplexType *previous_self = _current_self;
    _current_self = decl.owner;

    decl.value = rewrite_value_edge(decl.value);

    _current_self = previous_self;
    decl.expansion = ConstExpansion::t_expanded;
}

ConstDeclNode *ConstantExpander::resolve(ConstRefExprNode &ref)
{
    if (ref.is_self_qualified) {
        if (_current_self == nullptr) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(ref.token_name),
                "'self::' names a constant of the type it is written inside, and this is not inside one.");
            return nullptr;
        }

        Namespace *surface = member_surface_namespace(_collector.namespaces, *_current_self);
        ConstDeclNode *decl = surface != nullptr
            ? find_constant(_collector.namespaces, ref.token_name.value(), surface, /*qualified=*/true)
            : nullptr;

        if (decl == nullptr) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(ref.token_name),
                fmt::format(
                    "'{}' has no constant named '{}'.",
                    _current_self->name.value_or("<anonymous>"), ref.token_name.value()));
            return nullptr;
        }

        // a `self::` reference is inside the owner's own body, so this can only fail if the type itself
        // straddles modules - asked anyway, because the check belongs to resolution rather than to a spelling
        return refuse_if_later_module(*decl, ref) ? nullptr : decl;
    }

    ConstDeclNode *decl = find_constant(
        _collector.namespaces, ref.token_name.value(), ref.lookup_namespace, ref.is_qualified);

    if (decl == nullptr) {
        _collector.collect_issue<Issue::UnknownConstant>(
            code_ref_for(ref.token_name), ref.token_name.value());
        return nullptr;
    }

    return refuse_if_later_module(*decl, ref) ? nullptr : decl;
}

bool ConstantExpander::refuse_if_later_module(const ConstDeclNode &decl, const ConstRefExprNode &ref)
{
    if (module_may_name(decl)) {
        return false;
    }

    _collector.collect_issue<Issue::GenericError>(
        code_ref_for(ref.token_name),
        fmt::format(
            "'{}' is declared in the module '{}', which is compiled after '{}' - a module may only name "
            "symbols from one parsed before it. Move the constant into '{}' or into a module it depends on.",
            ref.token_name.value(), decl.declared_in_module->name, _current_module->name,
            decl.declared_in_module->name));

    return true;
}

ExprNode *ConstantExpander::rewrite_value_edge(ExprNode *expr)
{
    // the base's descent first, so a reference nested anywhere inside this expression is expanded before
    // this node is looked at
    expr = RecursiveVisitor::rewrite_value_edge(expr);

    if (expr == nullptr || expr->get_node_type() != NodeType::n_expr_const_ref) {
        return expr;
    }

    auto &ref = static_cast<ConstRefExprNode &>(*expr);

    ConstDeclNode *decl = resolve(ref);

    if (decl == nullptr) {
        // **removed from the tree, not merely reported.** a refused transient that stays reaches
        // PointerAdjuster's throw, which reports a compiler bug over the user's mistake. A void has an
        // accept() that does nothing, so every later walk stops at it
        return &_current_module->nodes.emplace_back<VoidExprNode>();
    }

    // the constant's own value, expanded, before it is copied anywhere - which is also what catches a cycle
    // reached for the first time through a use site rather than through the sweep above
    expand_initializer(*decl);

    if (decl->value == nullptr) {
        return &_current_module->nodes.emplace_back<VoidExprNode>();
    }

    // **into the walked module's arena**, never the declaring one's. A clone belongs to the unit that emits
    // it: putting an application's copy into a library's collection would make the library's object depend
    // on which applications consume it, which tests/module_cache.cpp compares byte for byte
    CloneContext cc(_current_module->nodes, _no_substitution, _collector.type_registry);

    Node *copy = decl->value->clone(cc);

    return static_cast<ExprNode *>(copy);
}

void ConstantExpander::visitFunctionDecl(FunctionDeclNode &node)
{
    ComplexType *previous_self = _current_self;

    // a member's body may write `self::MAX`; a free function's may not, and gets the null
    // AST::enclosing_type_of already answers with rather than whatever type happened to be walked
    // before it. that is the one owner of "which type is this body inside", shared with the `private`
    // rule - this file used to keep its own copy, and the two disagreed about a constructor
    _current_self = enclosing_type_of(node);

    RecursiveVisitor::visitFunctionDecl(node);

    _current_self = previous_self;
}

void ConstantExpander::visit_type_decl(TypeDeclNode &node)
{
    ComplexType *previous_self = _current_self;
    _current_self = &node.complex_type();

    RecursiveVisitor::visit_type_decl(node);

    _current_self = previous_self;
}

};
