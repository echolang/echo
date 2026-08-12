#include "AST/ASTAttributes.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <string_view>
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
    constexpr std::string_view k_declaration_attributes[] = {
        "inline", "implicit", "intrinsic", "builtin", "core", "unique" };

    // and the manifest's eight, which reach this parser too: a `module.eco` is **Echo**, read by the real
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
        "module", "version", "depends", "sources", "target", "link", "cc", "build_dir" };

    template <size_t N>
    bool contains(const std::string_view (&names)[N], const std::string &name)
    {
        return std::find(std::begin(names), std::end(names), name) != std::end(names);
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

bool AST::is_known_manifest_attribute(const std::string &name)
{
    return contains(k_manifest_attributes, name);
}

std::string AST::known_manifest_attribute_list()
{
    return fmt::format("{}", fmt::join(k_manifest_attributes, ", "));
}
