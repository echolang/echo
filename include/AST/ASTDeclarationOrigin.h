#ifndef ASTDECLARATIONORIGIN_H
#define ASTDECLARATIONORIGIN_H

#pragma once

#include <string>

namespace AST
{
    struct Context;
    class File;
    class Module;

    // **the module and file a declaration was written in**, recorded where it is knowable - while the
    // declaration is being parsed - rather than derived from a tree walk afterwards.
    //
    // this is the primary record and not a fourth cache. Three maps in the compiler answer a question of
    // this family today, each built by its own sweep over an arena or over the file roots
    // (Monomorphizer::_decl_module, OwnershipPass::_type_module, CodegenContext::function_file_map), and
    // every one of them is a *derivation* of what the parser already had in its hand and threw away. They
    // are still there; retiring them onto this field is the direction, and the reason to state it
    // here is that a fourth sweep added later would have nothing to be the fourth answer to.
    //
    // a **generic instantiation inherits its template's origin**, which falls out of
    // FunctionDeclNode::clone taking a shallow copy first - and it is the answer wanted rather than a
    // convenience: an instance is exactly as reachable as the template it came from, and the monomorphizer
    // appends its clone to `files().first()` of the template's module, so anything reading the *walk* to
    // find out where an instantiated body was written gets a file nobody wrote it in
    struct DeclarationOrigin
    {
        const Module *module = nullptr;
        const File *file = nullptr;

        // false for anything the compiler minted rather than read - a synthesized deinit, a copy
        // constructor, an instantiation whose template was itself minted. every visibility rule treats an
        // unknown origin as reaching everywhere, which is what keeps a compiler-minted call site out of a
        // refusal it could never have satisfied
        bool is_known() const {
            return module != nullptr;
        }

        bool same_file(const DeclarationOrigin &other) const {
            return file != nullptr && file == other.file;
        }

        bool same_module(const DeclarationOrigin &other) const {
            return module != nullptr && module == other.module;
        }
    };

    // the origin of whatever is being parsed right now. one function so no parser spells the pair by hand
    // and none of them can pick up only half of it
    DeclarationOrigin origin_at(const Context &context);
};

#endif
