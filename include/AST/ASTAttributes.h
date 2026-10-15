#ifndef ASTATTRIBUTES_H
#define ASTATTRIBUTES_H

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AST
{
    // **the set of attribute names the compiler reads**, and the one place it is spelled.
    //
    // three spellings, and the tier is in the spelling so the parser can refuse the wrong medium
    // without a second table:
    //
    //   #[export]                 the language - anyone, closed set
    //   #[echoc::builtin: size_of] compiler <-> stdlib - a compiler-supplied module, or any
    //                             module of a --no-stdlib build
    //   #[web::js_name: "x"]      a tool - anyone, carried, never read
    //
    // it exists because until it did, an attribute nobody read was accepted and silently ignored:
    // `#[intrinsc: "llvm.sqrt"]`, `#[bultin: "size_of"]` and `#[implict]` all parsed, attached to the
    // declaration and then did nothing at all - leaving a bodyless function with no implementation, or a
    // conversion that quietly was not one. What was validated was only an attribute's *payload*, once a
    // consumer had already found it by name.
    //
    // the second silence this closed: `#[depends: "../x"]` on a function in an ordinary `.eco` file
    // compiled and did nothing, because the known-name check was the union of the declaration and
    // manifest sets. `attribute_admission` is one question, one owner, and the medium is part of it

    namespace compiler_attribute
    {
        // one spelling of each compiler name, used by every lookup and message
        inline constexpr std::string_view core = "echoc::core";
        inline constexpr std::string_view builtin = "echoc::builtin";
        inline constexpr std::string_view intrinsic = "echoc::intrinsic";
    };

    enum class AttributeMedium
    {
        t_source,
        t_manifest
    };

    // may this name be written here. the switch has no tail. arm order is the content:
    //
    //   1. an `echoc::` name: t_reserved_namespace when not allowed, then t_compiler or
    //      t_unknown_compiler
    //   2. a tool name
    //   3. the medium's own closed set (t_language)
    //   4. the other medium's set: t_manifest_in_source. a declaration name inside a manifest
    //      stays t_unknown, which is what the manifest reader reports today
    //   5. otherwise t_unknown
    //
    // a bare `core`, `builtin` or `intrinsic` is always t_unknown, even when the module may write
    // the `echoc::` form. in a manifest, compiler_attributes_allowed is always false
    enum class AttributeAdmission
    {
        t_language,
        t_compiler,
        t_tool,
        t_reserved_namespace,
        t_unknown_compiler,
        t_manifest_in_source,
        t_unknown
    };

    AttributeAdmission attribute_admission(
        std::string_view name,
        AttributeMedium medium,
        bool compiler_attributes_allowed
    );

    // which declaration an attribute landed on. asked at the drain.
    // a `#[wasm: export]` object export is not a site: AttributeParser plants it
    // off the drain stack, and AST::check_wasm_surface is what reports it
    enum class AttributeSite
    {
        t_function,
        t_type,
        t_property,
        t_test
    };

    // empty when this name may sit here, otherwise the sentence
    std::optional<std::string> attribute_site_refusal(std::string_view name, AttributeSite site);

    // "a function or a type". empty for a name the table does not hold
    std::string attribute_site_list(std::string_view name);

    // the names themselves, in table order: the public declaration attributes (`#[inline]`) and the
    // manifest's (`#[module: ...]`). the language server's `#[` completion reads these so the
    // tables stay the one list. compiler names are not here: a program cannot write them
    std::vector<std::string_view> declaration_attribute_names();
    std::vector<std::string_view> manifest_attribute_names();

    // the accepted names, comma separated, for the "expected one of" in the diagnostic. Built from the same
    // list, so a name added there cannot be missing from the message that rejects its neighbours.
    // known_attribute_list is the public declaration names only
    std::string known_attribute_list();
    std::string known_manifest_attribute_list();
    std::string compiler_attribute_list();

    // a `<ns>::<name>` whose namespace echoc does not own. nullopt for a closed name, a reserved
    // `echoc::` name, or a malformed `::`. legal on a source declaration as well as a manifest:
    // codegen never reads one. `attribute_admission` is the reserved half
    std::optional<std::pair<std::string, std::string>> tool_attribute_name(const std::string &name);

    // what a `{ ... }` scope may do: a `#[target:]`'s or a `#[hook:]`'s. three answers, not a bool:
    // "inside one" and "carries one" are two properties, and only the owners have the second
    enum class AttributeScoping
    {
        // describes the module, so a scope has nothing to do with it: module, version, build_dir
        t_module_only,

        // may be written inside a scope, meaning there what it means at file scope: sources, depends,
        // link, cc
        t_scopable,

        // target and hook: they open a scope, so they cannot sit inside one
        t_opens_a_scope
    };

    // a column of the manifest table above rather than name comparisons at the point of use, so a ninth
    // manifest attribute states this once instead of being silently absent from three tests in a reader
    AttributeScoping manifest_attribute_scoping(const std::string &name);

    // who opened a `{ ... }` on a manifest. the parser's slot and the admission column are this
    // enum, so a third owner is a new arm rather than a string the diagnostic happens to print
    enum class ManifestScopeOwner
    {
        t_target,
        t_hook
    };

    // "target" / "hook": the word a refusal names, and the one `#[target:]` / `#[hook:]` already is
    const char *manifest_scope_owner_name(ManifestScopeOwner owner);

    // whether `owner`'s scope may hold `name`. a target holds everything scopable; a hook holds
    // only `depends` and `requires` (the tool's edges, never the program's)
    bool manifest_scope_admits(ManifestScopeOwner owner, const std::string &name);

    // what `owner`'s scope may hold, as the refusal names it
    std::string manifest_scope_admits_list(ManifestScopeOwner owner);
};

#endif
