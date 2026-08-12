#ifndef ASTATTRIBUTES_H
#define ASTATTRIBUTES_H

#pragma once

#include <string>

namespace AST
{
    // **the set of attribute names the compiler reads**, and the one place it is spelled.
    //
    // it exists because until it did, an attribute nobody read was accepted and silently ignored:
    // `#[intrinsc: "llvm.sqrt"]`, `#[bultin: "size_of"]` and `#[implict]` all parsed, attached to the
    // declaration and then did nothing at all - leaving a bodyless function with no implementation, or a
    // conversion that quietly was not one. What was validated was only an attribute's *payload*, once a
    // consumer had already found it by name.
    //
    // conditional compilation is what made that intolerable rather than merely untidy. A misspelled
    // `#[fi: os == darwin]` passes straight through Parser::filter_conditional_tokens - it is not one of
    // the four directives - and would then be dropped here without a word, taking a whole platform's block
    // with it in the one direction nothing else in the compiler would ever notice.
    //
    // a *closed* set, the rule the manifest reader has always lived by: `#[sources:]` misspelled
    // `#[source:]` would otherwise produce a module with no files and no complaint.
    bool is_known_attribute(const std::string &name);

    // the accepted names, comma separated, for the "expected one of" in the diagnostic. Built from the same
    // list, so a name added there cannot be missing from the message that rejects its neighbours
    std::string known_attribute_list();

    // **the manifest's own narrower set**, and the same two questions over it. Here rather than private to
    // Parser::ManifestParser because the union above has to contain it - a `module.eco` is Echo, read by
    // this same attribute parser - and two hand-maintained copies of four names is exactly the drift the
    // union was added to close: a fifth manifest key added to one array and not the other is silent in the
    // direction nothing catches
    bool is_known_manifest_attribute(const std::string &name);
    std::string known_manifest_attribute_list();

    // what a manifest attribute may do with a `#[target: ...] { ... }` scope.
    //
    // **three answers and not a bool**, because "may be written inside one" and "may carry one" are two
    // properties and exactly one attribute has the second. A `#[target:]` written *inside* a scope is the
    // case a pair of booleans loses: it neither carries a brace of its own nor belongs to another target,
    // and a reader asking only "is this scopable" accepts it and hangs a second module-level target off it
    enum class AttributeScoping
    {
        // describes the module, so a scope has nothing to do with it: module, version, build_dir
        t_module_only,

        // may be written inside a scope, meaning there what it means at file scope: sources, depends,
        // link, cc
        t_scopable,

        // the one that opens a scope, and therefore may not be written inside one either
        t_opens_a_scope
    };

    // a column of the manifest table above rather than name comparisons at the point of use, so a ninth
    // manifest attribute states this once instead of being silently absent from three tests in a reader
    AttributeScoping manifest_attribute_scoping(const std::string &name);
};

#endif
