#include "AST/ASTAttributes.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    // every attribute a *declaration* consumer looks up by name, and nothing else.
    //
    //   inline      FuncDeclParser  -> FunctionDeclNode::is_inline
    //   implicit    FuncDeclParser  -> publish_implicit_conversion
    //   intrinsic   FuncDeclParser  -> FunctionDeclNode::intrinsic
    //   builtin     FuncDeclParser  -> FunctionDeclNode::builtin, checked by AST::is_known_builtin
    //   core        TypeDeclParser  -> bind_core_type_attribute, checked by AST::core_type_kind_for
    //   unique      TypeDeclParser  -> bind_unique_attribute, read by AST::classify_copy
    //   group       TestDeclParser  -> AST::TestDeclaration::group, read by Compiler::select_tests
    constexpr std::string_view k_declaration_attributes[] = {
        "inline", "implicit", "intrinsic", "builtin", "core", "unique", "group" };

    // and the manifest's nine, which reach this parser too: a `module.eco` is **Echo**, read by the real
    // lexer and the real attribute parser into a scratch bundle. Leaving them out of the union made every
    // manifest in the tree report four unknown attributes at once.
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
    bool contains(const std::string_view (&names)[N], const std::string &name)
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

bool AST::is_known_attribute(const std::string &name)
{
    // the union, which is the only question one parser shared by both grammars can answer: "is this a name
    // the compiler reads *anywhere*". Which names are legal *in a manifest* is the narrower set below, and
    // that is what still refuses `#[inline]` in a module.eco.
    //
    // the four conditional directives are deliberately **not** here, and do not need to be:
    // Parser::filter_conditional_tokens consumes every one of them - including those nested inside a region
    // it is dropping, since it reads the structure whether or not it is emitting - so none can reach this
    // parser. Listing them would be a second, unreachable spelling of that list
    return contains(k_declaration_attributes, name) || contains(k_manifest_attributes, name);
}

std::string AST::known_attribute_list()
{
    return fmt::format("{}, {}",
        fmt::join(k_declaration_attributes, ", "), fmt::join(k_manifest_attributes, ", "));
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

bool AST::is_reserved_manifest_namespace(const std::string &name)
{
    std::string_view ns;
    std::string_view rest;

    return split_tool_attribute_name(name, ns, rest) && ns == "echoc";
}

bool AST::is_known_manifest_attribute(const std::string &name)
{
    return contains(k_manifest_attributes, name) || tool_attribute_name(name).has_value();
}

std::string AST::known_manifest_attribute_list()
{
    return fmt::format("{}", fmt::join(k_manifest_attributes, ", "));
}

AST::AttributeScoping AST::manifest_attribute_scoping(const std::string &name)
{
    const auto found = std::find_if(
        std::begin(k_manifest_scoping), std::end(k_manifest_scoping),
        [&name](const auto &row) { return row.first == name; });

    // a name missing from the scoping table is `t_module_only`. that is the right answer for a
    // tool-namespace attribute (`#[epm::license:]`), which is accepted by is_known_manifest_attribute
    // without a row here. it is also the arm that says a closed name was added to k_manifest_attributes
    // and not to this table
    return found == std::end(k_manifest_scoping)
        ? AST::AttributeScoping::t_module_only : found->second;
}
