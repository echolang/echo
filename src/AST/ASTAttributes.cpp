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
    //   wasm        FuncDeclParser drains `#[wasm: import]` onto an extern function.
    //               a `#[wasm: export]` object export is planted off the drain stack
    //               by AttributeParser. AST::check_wasm_surface reports.
    constexpr unsigned site_bit(AST::AttributeSite site)
    {
        return 1u << static_cast<unsigned>(site);
    }

    constexpr unsigned k_on_function = site_bit(AST::AttributeSite::t_function);
    constexpr unsigned k_on_type = site_bit(AST::AttributeSite::t_type);
    constexpr unsigned k_on_test = site_bit(AST::AttributeSite::t_test);

    struct DeclarationAttribute
    {
        std::string_view name;
        unsigned sites;
    };

    constexpr DeclarationAttribute k_declaration_attributes[] = {
        { "inline",   k_on_function },
        { "implicit", k_on_function },
        { "unique",   k_on_type },
        { "atomic",   k_on_type },
        { "group",    k_on_test },
        { "tests",    k_on_test },
        { "export",   k_on_function | k_on_type },
        { "wasm",     k_on_function },
    };

    // the compiler's private contract with its stdlib. one spelling, used by every lookup and
    // message. gated on AST::Module::writes_compiler_attributes
    constexpr DeclarationAttribute k_compiler_attributes[] = {
        { AST::compiler_attribute::core,      k_on_type },
        { AST::compiler_attribute::builtin,   k_on_function },
        { AST::compiler_attribute::intrinsic, k_on_function },
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

    template <size_t N>
    const DeclarationAttribute *find_row(const DeclarationAttribute (&rows)[N], std::string_view name)
    {
        const auto found = std::find_if(std::begin(rows), std::end(rows), [name](const DeclarationAttribute &row) {
            return row.name == name;
        });

        return found == std::end(rows) ? nullptr : found;
    }

    template <size_t N>
    std::vector<std::string_view> names_of(const DeclarationAttribute (&rows)[N])
    {
        std::vector<std::string_view> names;
        for (const DeclarationAttribute &row : rows) {
            names.push_back(row.name);
        }
        return names;
    }

    const char *site_noun(AST::AttributeSite site)
    {
        switch (site) {
            case AST::AttributeSite::t_function: return "a function";
            case AST::AttributeSite::t_type: return "a type";
            case AST::AttributeSite::t_property: return "a property";
            case AST::AttributeSite::t_test: return "a test";
        }

        assert(false && "unhandled AttributeSite in site_noun");
        return "a declaration";
    }

    // the sites a row admits, as prose ("a function or a type")
    std::string site_list(unsigned sites)
    {
        std::vector<std::string_view> nouns;
        for (AST::AttributeSite site : { AST::AttributeSite::t_function, AST::AttributeSite::t_type,
                                         AST::AttributeSite::t_property, AST::AttributeSite::t_test }) {
            if ((sites & site_bit(site)) != 0) {
                nouns.push_back(site_noun(site));
            }
        }

        return fmt::format("{}", fmt::join(nouns, " or "));
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
    return names_of(k_declaration_attributes);
}

std::vector<std::string_view> AST::manifest_attribute_names()
{
    return std::vector<std::string_view>(std::begin(k_manifest_attributes), std::end(k_manifest_attributes));
}

std::string AST::known_attribute_list()
{
    return fmt::format("{}", fmt::join(names_of(k_declaration_attributes), ", "));
}

std::string AST::known_manifest_attribute_list()
{
    return fmt::format("{}", fmt::join(k_manifest_attributes, ", "));
}

std::string AST::compiler_attribute_list()
{
    return fmt::format("{}", fmt::join(names_of(k_compiler_attributes), ", "));
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

            return find_row(k_compiler_attributes, name) != nullptr
                ? AttributeAdmission::t_compiler
                : AttributeAdmission::t_unknown_compiler;
        }

        return AttributeAdmission::t_tool;
    }

    switch (medium) {
    case AttributeMedium::t_source:
        if (find_row(k_declaration_attributes, name) != nullptr) {
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

std::string AST::attribute_site_list(std::string_view name)
{
    const DeclarationAttribute *row = find_row(k_declaration_attributes, name);

    if (row == nullptr) {
        row = find_row(k_compiler_attributes, name);
    }

    return row == nullptr ? std::string() : site_list(row->sites);
}

std::optional<std::string> AST::attribute_site_refusal(std::string_view name, AST::AttributeSite site)
{
    const DeclarationAttribute *row = find_row(k_declaration_attributes, name);

    if (row == nullptr) {
        row = find_row(k_compiler_attributes, name);
    }

    // a tool attribute, or a name admission already refused
    if (row == nullptr || (row->sites & site_bit(site)) != 0) {
        return std::nullopt;
    }

    return fmt::format(
        "'#[{}]' cannot be written on {}. It belongs on {}.", name, site_noun(site), site_list(row->sites));
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
