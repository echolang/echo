#include "AST/ASTAttributes.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cassert>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    // every attribute a *declaration* consumer looks up by name, and nothing the compiler keeps
    // for its standard library. those three live in k_compiler_attributes, spelled `echoc::`.
    //
    //   inline      FuncDeclParser  -> FunctionDeclNode::is_inline
    //   implicit    FuncDeclParser  -> publish_implicit_conversion
    //   unique      TypeDeclParser  -> bind_unique_attribute, read by AST::classify_copy
    //   atomic      TypeDeclParser  -> bind_atomic_attribute, read by AST::counts_are_atomic
    //   group       TestDeclParser  -> AST::TestDeclaration::group, read by Compiler::select_tests
    //   tests       TestDeclParser  -> AST::TestDeclaration::expects_death, read by
    //               Compiler::run_test_isolated. not the `tests` *condition flag*
    //               (TargetFacts::tests / `#[if: tests]`): that is a directive value, this is an
    //               attribute name. directives never reach attribute_admission
    //   export      FuncDeclParser  -> FunctionDeclNode::export_name, AST::check_exports.
    //               on a type that answers an extern interface: AST::type_exports_abi_table,
    //               AST::check_abi_exports, AbiCodegen::emit_tables
    //   wasm        Parser::parse_attribute plants object exports; AST::check_wasm_surface
    //               seats import_module on an extern, or a file-root object export (`memory`)
    constexpr std::string_view k_declaration_attributes[] = {
        "inline", "implicit", "unique", "atomic", "group", "tests", "export", "wasm" };

    // the compiler's private contract with its stdlib. one spelling, used by every lookup and
    // message. gated on AST::Module::writes_compiler_attributes
    constexpr std::string_view k_compiler_attributes[] = {
        AST::compiler_attribute::core,
        AST::compiler_attribute::builtin,
        AST::compiler_attribute::intrinsic
    };

    // and the manifest's nine, which reach this parser too: a `module.eco` is **Echo**, read by the real
    // lexer and the real attribute parser into a scratch bundle.
    //
    //   module / version / depends / sources    Parser::read_manifest_attributes
    //   target                                  -> Parser::read_manifest_targets
    //   link                                    -> Compiler::parse_link_requirement
    //   cc                                      -> Compiler::parse_cc_requirement
    //   build_dir                               -> Compiler::BuildLayout
    //
    // link, cc and target carry a tag inside their value rather than being one attribute per kind, so what
    // a build may need linked - or produce - can grow without this list growing with it, which is also why
    // neither name says what it is for beyond the tool it reaches
    constexpr std::string_view k_manifest_attributes[] = {
        "module", "version", "depends", "sources", "target", "link", "cc", "build_dir", "requires" };

    // and what each of them may do with a `{ ... }` scope. **A column of the table above rather than name
    // comparisons at the point of use**: a ninth manifest attribute has to state this or it does not
    // compile, where three `name == "..."` tests in a reader are three places a ninth name is simply
    // absent from - and the one it is absent from accepts it in silence
    constexpr std::pair<std::string_view, AST::AttributeScoping> k_manifest_scoping[] = {
        { "module",    AST::AttributeScoping::t_module_only },
        { "version",   AST::AttributeScoping::t_module_only },
        { "build_dir", AST::AttributeScoping::t_module_only },
        { "target",    AST::AttributeScoping::t_opens_a_scope },
        { "depends",   AST::AttributeScoping::t_scopable },
        { "sources",   AST::AttributeScoping::t_scopable },
        { "link",      AST::AttributeScoping::t_scopable },
        { "cc",        AST::AttributeScoping::t_scopable },
        { "requires",  AST::AttributeScoping::t_scopable },
    };

    template <size_t N>
    bool contains(const std::string_view (&names)[N], std::string_view name)
    {
        return std::find(std::begin(names), std::end(names), name) != std::end(names);
    }

    bool split_tool_attribute_name(
        std::string_view name,
        std::string_view &ns,
        std::string_view &rest)
    {
        const size_t sep = name.find("::");

        if (sep == 0 || sep == std::string::npos || sep + 2 >= name.size()) {
            return false;
        }

        if (name.find(":::") != std::string::npos || name.ends_with("::")) {
            return false;
        }

        ns = name.substr(0, sep);
        rest = name.substr(sep + 2);
        return !ns.empty() && !rest.empty();
    }
};

std::vector<std::string_view> AST::declaration_attribute_names()
{
    return std::vector<std::string_view>(std::begin(k_declaration_attributes), std::end(k_declaration_attributes));
}

std::vector<std::string_view> AST::manifest_attribute_names()
{
    return std::vector<std::string_view>(std::begin(k_manifest_attributes), std::end(k_manifest_attributes));
}

std::string AST::known_attribute_list()
{
    return fmt::format("{}", fmt::join(k_declaration_attributes, ", "));
}

std::string AST::known_manifest_attribute_list()
{
    return fmt::format("{}", fmt::join(k_manifest_attributes, ", "));
}

std::string AST::compiler_attribute_list()
{
    return fmt::format("{}", fmt::join(k_compiler_attributes, ", "));
}

std::optional<std::pair<std::string, std::string>> AST::tool_attribute_name(const std::string &name)
{
    std::string_view ns;
    std::string_view rest;

    if (!split_tool_attribute_name(name, ns, rest) || ns == "echoc") {
        return std::nullopt;
    }

    return std::make_pair(std::string(ns), std::string(rest));
}

AST::AttributeAdmission AST::attribute_admission(
    std::string_view name,
    AST::AttributeMedium medium,
    bool compiler_attributes_allowed
)
{
    std::string_view ns;
    std::string_view rest;

    if (split_tool_attribute_name(name, ns, rest)) {
        if (ns == "echoc") {
            if (!compiler_attributes_allowed) {
                return AttributeAdmission::t_reserved_namespace;
            }

            return contains(k_compiler_attributes, name)
                ? AttributeAdmission::t_compiler
                : AttributeAdmission::t_unknown_compiler;
        }

        return AttributeAdmission::t_tool;
    }

    switch (medium) {
    case AttributeMedium::t_source:
        if (contains(k_declaration_attributes, name)) {
            return AttributeAdmission::t_language;
        }

        if (contains(k_manifest_attributes, name)) {
            return AttributeAdmission::t_manifest_in_source;
        }

        return AttributeAdmission::t_unknown;

    case AttributeMedium::t_manifest:
        if (contains(k_manifest_attributes, name)) {
            return AttributeAdmission::t_language;
        }

        return AttributeAdmission::t_unknown;
    }

    // no tail: a third medium is a compile error here rather than a name that silently attaches
    assert(false && "unhandled AttributeMedium in attribute_admission");
    return AttributeAdmission::t_unknown;
}

AST::AttributeScoping AST::manifest_attribute_scoping(const std::string &name)
{
    const auto found = std::find_if(
        std::begin(k_manifest_scoping), std::end(k_manifest_scoping),
        [&name](const auto &row) { return row.first == name; });

    // a name missing from the scoping table is `t_module_only`. that is the right answer for a
    // tool-namespace attribute (`#[epm::license:]`), which is accepted by attribute_admission
    // without a row here. it is also the arm that says a closed name was added to k_manifest_attributes
    // and not to this table
    return found == std::end(k_manifest_scoping)
        ? AST::AttributeScoping::t_module_only : found->second;
}
