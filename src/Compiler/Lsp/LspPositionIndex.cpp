#include "Compiler/Lsp/LspPositionIndex.h"

#include "AST/ASTBundle.h"
#include "AST/ASTFile.h"
#include "AST/ASTNode.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTSourceToken.h"
#include "AST/ConstDeclNode.h"
#include "AST/ConstRefExprNode.h"
#include "AST/GenericValueExprNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/StaticPropertyExprNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "Compiler/SettledPath.h"
#include "Token.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace
{
    bool token_is_indexable(const TokenReference *token)
    {
        return token != nullptr && token->is_valid() && !token->is_minted();
    }

    bool same_file_token(const TokenReference &token, const AST::File *file)
    {
        return token.is_valid() && token.file() == file;
    }

    std::optional<AST::Span> call_paren_span(const AST::FunctionCallExprNode &call)
    {
        if (!token_is_indexable(&call.token_function_name)) {
            return std::nullopt;
        }

        const AST::File *file = call.token_function_name.file();
        const TokenCollection &tokens = call.token_function_name.get_collection_ref();
        size_t index = call.token_function_name.get_handle() + 1;
        int angles = 0;

        while (index < tokens.tokens.size()) {
            const TokenReference token(tokens, index);
            if (!same_file_token(token, file)) {
                break;
            }

            const Token::Type type = token.type();
            if (type == Token::Type::t_open_angle) {
                angles++;
            }
            else if (type == Token::Type::t_close_angle) {
                angles--;
            }
            else if (type == Token::Type::t_op_shr) {
                angles -= 2;
            }
            else if (type == Token::Type::t_open_paren && angles <= 0) {
                const size_t open = index;
                int parens = 1;
                size_t close = open;
                index++;
                while (index < tokens.tokens.size() && parens > 0) {
                    const TokenReference inner(tokens, index);
                    if (!same_file_token(inner, file)) {
                        return std::nullopt;
                    }

                    if (inner.type() == Token::Type::t_open_paren) {
                        parens++;
                    }
                    else if (inner.type() == Token::Type::t_close_paren) {
                        parens--;
                        close = index;
                    }

                    index++;
                }

                if (parens != 0) {
                    return std::nullopt;
                }

                return AST::union_span(
                    AST::span_of(TokenReference(tokens, open)),
                    AST::span_of(TokenReference(tokens, close)));
            }

            index++;
        }

        return std::nullopt;
    }

    AST::Span argument_span(const AST::ExprNode *expr)
    {
        if (expr == nullptr) {
            return {};
        }

        const TokenReference *token = AST::source_token_of(*expr);
        if (token == nullptr || !token->is_valid()) {
            return {};
        }

        return AST::span_of(*token);
    }

    AST::Span call_span_of(AST::FunctionCallExprNode &call)
    {
        AST::Span span = AST::span_of(call.token_function_name);
        if (const std::optional<AST::Span> parens = call_paren_span(call)) {
            return AST::union_span(span, parens.value());
        }

        for (AST::ExprNode *arg : call.arguments) {
            span = AST::union_span(span, argument_span(arg));
        }

        return span;
    }

    std::string path_key(const std::filesystem::path &path)
    {
        return Compiler::canonical_or_absolute(path).generic_string();
    }

    class IndexBuilder : public AST::RecursiveVisitor
    {
    public:

        IndexBuilder(
            std::vector<Compiler::Lsp::PositionIndex::Entry> &into,
            std::vector<Compiler::Lsp::PositionIndex::CallSite> *calls
        ) :
            _into(into),
            _calls(calls)
        {}

        void visitVar(AST::VarNode &node) override
        {
            record(node.use_token(), &node);
            AST::RecursiveVisitor::visitVar(node);
        }

        void visitFunctionCallExpr(AST::FunctionCallExprNode &node) override
        {
            // implicit conversions and retains reuse the author's token; recording
            // them would map a click on a definition onto a synthesised call
            if (!node.is_implcit) {
                record(node.token_function_name, &node);
                if (_calls != nullptr) {
                    Compiler::Lsp::PositionIndex::CallSite site;
                    site.call = &node;
                    site.span = call_span_of(node);
                    _calls->push_back(site);
                }
            }

            for (AST::TypeNode *arg : node.explicit_type_args) {
                index_type(arg);
            }

            AST::RecursiveVisitor::visitFunctionCallExpr(node);
        }

        void visitVarDecl(AST::VarDeclNode &node) override
        {
            record(node.token_varname, &node);
            index_type(node.optional_type_node());
            AST::RecursiveVisitor::visitVarDecl(node);
        }

        void visitFunctionDecl(AST::FunctionDeclNode &node) override
        {
            // instantiations share the template's tokens; walking them remaps those
            // tokens onto whichever instance was last. skip them entirely
            if (node.is_instantiated()) {
                return;
            }

            if (node.name_token.has_value()) {
                record(node.name_token.value(), &node);
            }

            index_type(node.return_type);
            AST::RecursiveVisitor::visitFunctionDecl(node);
        }

        void visit_type_decl(AST::TypeDeclNode &node) override
        {
            if (node.name_token.has_value()) {
                record(node.name_token.value(), &node);
            }

            AST::RecursiveVisitor::visit_type_decl(node);
        }

        void visit_const_decl(AST::ConstDeclNode &node) override
        {
            record(node.token_name, &node);
            AST::RecursiveVisitor::visit_const_decl(node);
        }

        void visit_const_ref(AST::ConstRefExprNode &node) override
        {
            record(node.token_name, &node);
            AST::RecursiveVisitor::visit_const_ref(node);
        }

        void visit_generic_value(AST::GenericValueExprNode &node) override
        {
            record(node.token_name, &node);
            AST::RecursiveVisitor::visit_generic_value(node);
        }

        void visitType(AST::TypeNode &node) override
        {
            if (node.written_names.empty() && node.type_token.has_value()) {
                record(node.type_token.value(), &node);
            }

            AST::RecursiveVisitor::visitType(node);
        }

        void visitMemberAccess(AST::MemberAccessNode &node) override
        {
            record(node.get_member_name(), &node);
            AST::RecursiveVisitor::visitMemberAccess(node);
        }

        void visit_static_property(AST::StaticPropertyExprNode &node) override
        {
            record(node.token_name, &node);
            AST::RecursiveVisitor::visit_static_property(node);
        }

        void visit_function_ref_expr(AST::FunctionRefExprNode &node) override
        {
            record(node.token_name, &node);
            AST::RecursiveVisitor::visit_function_ref_expr(node);
        }

    private:

        std::vector<Compiler::Lsp::PositionIndex::Entry> &_into;
        std::vector<Compiler::Lsp::PositionIndex::CallSite> *_calls;

        void index_type(AST::TypeNode *type)
        {
            if (type != nullptr) {
                type->accept(*this);
            }
        }

        void record(const TokenReference &token, AST::Node *node)
        {
            if (!token_is_indexable(&token)) {
                return;
            }

            Compiler::Lsp::PositionIndex::Entry entry;
            entry.line = token.line();
            entry.column = token.char_offset();
            entry.width = static_cast<uint32_t>(token.value().size());
            if (entry.width == 0) {
                entry.width = 1;
            }
            entry.node = node;
            _into.push_back(entry);
        }
    };
};

void Compiler::Lsp::PositionIndex::build(AST::Bundle &bundle)
{
    _by_file.clear();
    _calls.clear();
    _by_path.clear();

    for (auto &module_ptr : bundle.modules) {
        AST::Module &module = *module_ptr;

        for (AST::File &file : module.files()) {
            _by_path[path_key(file.get_path())] = &file;

            std::vector<Entry> entries;
            std::vector<CallSite> calls;

            if (file.root != nullptr) {
                IndexBuilder builder(entries, &calls);
                file.root->accept(builder);
            }

            // constants live in the arena, not the file root. pick up the ones written here
            IndexBuilder builder(entries, nullptr);
            for (AST::ConstDeclNode *decl : module.nodes.of_type<AST::ConstDeclNode>()) {
                const AST::File *home = decl->declared_in.file;
                if (home == nullptr && decl->token_name.is_valid()) {
                    home = decl->token_name.file();
                }

                if (home != &file) {
                    continue;
                }

                decl->accept(builder);
            }

            // ConstantExpander replaces each use with a clone of the initializer, so the
            // ConstRefExprNode is gone from the tree but still in the arena - and still holds
            // the name token the author wrote
            for (AST::ConstRefExprNode *ref : module.nodes.of_type<AST::ConstRefExprNode>()) {
                if (ref->token_name.is_valid() && ref->token_name.file() == &file) {
                    ref->accept(builder);
                }
            }

            std::stable_sort(entries.begin(), entries.end(),
                [](const Entry &a, const Entry &b) {
                    if (a.line != b.line) {
                        return a.line < b.line;
                    }
                    return a.column < b.column;
                });

            _by_file[&file] = std::move(entries);
            _calls[&file] = std::move(calls);
        }
    }
}

const Compiler::Lsp::PositionIndex::Entry *Compiler::Lsp::PositionIndex::entry_at(
    const AST::File *file,
    uint32_t line,
    uint32_t column
) const
{
    auto found = _by_file.find(file);
    if (found == _by_file.end()) {
        return nullptr;
    }

    const std::vector<Entry> &entries = found->second;

    Entry probe;
    probe.line = line;
    probe.column = column + 1;

    auto it = std::upper_bound(entries.begin(), entries.end(), probe,
        [](const Entry &a, const Entry &b) {
            if (a.line != b.line) {
                return a.line < b.line;
            }
            return a.column < b.column;
        });

    const Entry *use = nullptr;
    const Entry *decl = nullptr;

    while (it != entries.begin()) {
        --it;

        if (it->line != line) {
            break;
        }

        if (column >= it->column && column < it->column + it->width) {
            const AST::NodeReference ref = AST::make_ref(it->node);
            const bool is_decl = ref.has_type<AST::FunctionDeclNode>()
                || ref.has_type<AST::TypeDeclNode>()
                || ref.has_type<AST::VarDeclNode>()
                || ref.has_type<AST::ConstDeclNode>();

            if (is_decl) {
                decl = &*it;
                break;
            }

            if (use == nullptr) {
                use = &*it;
            }
        }
    }

    return decl != nullptr ? decl : use;
}

AST::Node *Compiler::Lsp::PositionIndex::at(
    const AST::File *file,
    uint32_t line,
    uint32_t column
) const
{
    const Entry *hit = entry_at(file, line, column);
    return hit != nullptr ? hit->node : nullptr;
}

const AST::File *Compiler::Lsp::PositionIndex::file_for_path(const std::filesystem::path &path) const
{
    auto found = _by_path.find(path_key(path));
    if (found != _by_path.end()) {
        return found->second;
    }

    return nullptr;
}

std::vector<const AST::File *> Compiler::Lsp::PositionIndex::files() const
{
    std::vector<const AST::File *> out;
    out.reserve(_by_file.size());
    for (const auto &[file, entries] : _by_file) {
        if (file != nullptr) {
            out.push_back(file);
        }
    }

    return out;
}

std::vector<std::string> Compiler::Lsp::PositionIndex::paths() const
{
    std::vector<std::string> out;
    out.reserve(_by_file.size());
    for (const auto &[file, entries] : _by_file) {
        if (file != nullptr) {
            out.push_back(path_key(file->get_path()));
        }
    }

    return out;
}

void Compiler::Lsp::PositionIndex::visit_entries(
    const std::function<void(const AST::File &, const Entry &)> &fn
) const
{
    for (const auto &[file, entries] : _by_file) {
        if (file == nullptr) {
            continue;
        }

        for (const Entry &entry : entries) {
            fn(*file, entry);
        }
    }
}

AST::FunctionCallExprNode *Compiler::Lsp::PositionIndex::enclosing_call(
    const AST::File *file,
    uint32_t line,
    uint32_t column
) const
{
    auto found = _calls.find(file);
    if (found == _calls.end()) {
        return nullptr;
    }

    const AST::Location location{ line, column };
    AST::FunctionCallExprNode *best = nullptr;
    AST::Span best_span;

    for (const CallSite &site : found->second) {
        if (site.call == nullptr || site.call->decl == nullptr || site.span.file == nullptr) {
            continue;
        }

        if (!AST::location_in_span(location, site.span)) {
            continue;
        }

        if (best == nullptr
            || AST::location_before(best_span.start, site.span.start)
            || (site.span.start.line == best_span.start.line
                && site.span.start.column == best_span.start.column
                && AST::location_before(site.span.end, best_span.end))) {
            best = site.call;
            best_span = site.span;
        }
    }

    return best;
}
