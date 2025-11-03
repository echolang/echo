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
};

#endif
