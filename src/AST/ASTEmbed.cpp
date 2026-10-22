#include "AST/ASTEmbed.h"

#include "AST/ASTCollector.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTFile.h"
#include "AST/ASTModule.h"
#include "AST/ASTStringLiteral.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "Compiler/ModuleCache.h"

#include <fmt/core.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace
{

std::string embed_name(const AST::FunctionCallExprNode &call)
{
    if (call.decl != nullptr) {
        return call.decl->namespaced_func_name();
    }

    return call.token_function_name.value();
}

std::string embed_bytes_spelling(const AST::FunctionCallExprNode &call, const AST::Collector *collector)
{
    // the other embed builtin in this namespace, so a rename of `bytes` still names it
    if (collector == nullptr || call.decl == nullptr || call.decl->ast_namespace == nullptr) {
        return "bytes";
    }

    for (AST::FunctionDeclNode *candidate : collector->functions.get_all()) {
        if (candidate == nullptr
            || candidate->ast_namespace != call.decl->ast_namespace
            || !candidate->is_builtin()) {
            continue;
        }

        if (AST::builtin_kind_for(candidate->builtin.value()) == AST::BuiltinKind::t_embed_bytes) {
            return candidate->namespaced_func_name();
        }
    }

    return "bytes";
}

std::filesystem::path canonical_or_normal(const std::filesystem::path &path)
{
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);

    if (!ec) {
        return canonical;
    }

    return std::filesystem::absolute(path).lexically_normal();
}

bool path_is_inside(const std::filesystem::path &candidate, const std::filesystem::path &bound)
{
    const std::filesystem::path a = canonical_or_normal(candidate);
    const std::filesystem::path b = canonical_or_normal(bound);

    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(a, b, ec);

    if (ec) {
        return false;
    }

    for (const std::filesystem::path &part : relative) {
        if (part == "..") {
            return false;
        }
    }

    return true;
}

AST::EmbedPlan pending()
{
    AST::EmbedPlan plan;
    plan.status = AST::EmbedStatus::t_pending;
    return plan;
}

AST::EmbedPlan refused(std::string sentence)
{
    AST::EmbedPlan plan;
    plan.status = AST::EmbedStatus::t_refused;
    plan.refusal = std::move(sentence);
    return plan;
}

}

bool AST::is_embed_builtin(AST::BuiltinKind kind)
{
    return kind == AST::BuiltinKind::t_embed_text || kind == AST::BuiltinKind::t_embed_bytes;
}

AST::EmbedKind AST::embed_kind_of(AST::BuiltinKind kind)
{
    return kind == AST::BuiltinKind::t_embed_text ? AST::EmbedKind::t_text : AST::EmbedKind::t_bytes;
}

AST::EmbedPlan AST::embed_plan_for(AST::FunctionCallExprNode &call, const AST::Collector *collector)
{
    if (call.decl == nullptr || !call.decl->is_builtin()) {
        return pending();
    }

    const AST::BuiltinKind kind = AST::builtin_kind_for(call.decl->builtin.value());

    if (!is_embed_builtin(kind)) {
        return pending();
    }

    if (call.decl->is_generic()) {
        return pending();
    }

    const AST::EmbedKind embed_kind = embed_kind_of(kind);
    const std::string name = embed_name(call);

    if (call.arguments.empty() || call.arguments[0] == nullptr) {
        return pending();
    }

    const std::optional<std::string> written = AST::literal_string_value(call.arguments[0]);

    if (!written.has_value()) {
        return refused(fmt::format(
            "'{}' needs a string literal as its path, not a runtime value", name));
    }

    const std::string &path_text = written.value();

    if (path_text.empty()) {
        return refused(fmt::format("'{}' needs a path, not an empty string", name));
    }

    if (path_text.find('\\') != std::string::npos) {
        return refused(fmt::format(
            "'{}' paths use '/', not a backslash", name));
    }

    const std::filesystem::path relative(path_text);

    // `is_absolute()` on Windows wants a drive; `/etc/passwd` only has a root
    // directory, and the jail would then say "outside this module"
    if (relative.has_root_name() || relative.has_root_directory()) {
        return refused(fmt::format(
            "'{}' is an absolute path; embed paths are relative to this file", path_text));
    }

    AST::File *source = call.token_function_name.file();

    if (source == nullptr) {
        return pending();
    }

    AST::Module *store = source->module;

    if (store == nullptr) {
        return pending();
    }

    const std::filesystem::path source_dir = source->get_path().parent_path();
    const std::filesystem::path resolved = canonical_or_normal(source_dir / relative);
    const std::filesystem::path bound = store->directory.empty()
        ? source_dir
        : store->directory;

    if (!path_is_inside(resolved, bound)) {
        return refused(fmt::format("'{}' is outside this module", path_text));
    }

    std::error_code ec;

    if (std::filesystem::is_directory(resolved, ec) && !ec) {
        return refused(fmt::format(
            "'{}' is a directory, and '{}' reads one file", path_text, name));
    }

    const std::string key = resolved.generic_string();
    AST::EmbeddedResource *resource = nullptr;
    auto found = store->embed_bytes.find(key);

    if (found != store->embed_bytes.end()) {
        resource = found->second.get();
    }

    if (resource == nullptr) {
        const std::optional<std::string> bytes = Compiler::read_whole_file(resolved);

        if (!bytes.has_value()) {
            return refused(fmt::format("'{}' is not a file", path_text));
        }

        auto interned = std::make_unique<AST::EmbeddedResource>();
        interned->resolved = resolved;
        interned->bytes = bytes.value();
        resource = interned.get();
        store->embed_bytes.emplace(key, std::move(interned));
    }

    if (embed_kind == AST::EmbedKind::t_text && AST::utf8_first_invalid(resource->bytes)) {
        const AST::CoreTypes unbound;
        const AST::CoreTypes &types = collector != nullptr ? collector->core_types : unbound;
        return refused(fmt::format(
            "'{}' is not valid UTF-8, so it cannot be a '{}' - use '{}'",
            path_text,
            types.spelling(AST::CoreTypeKind::t_string),
            embed_bytes_spelling(call, collector)));
    }

    AST::EmbedPlan plan;
    plan.status = AST::EmbedStatus::t_ready;
    plan.kind = embed_kind;
    plan.resource = resource;
    return plan;
}
