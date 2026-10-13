#ifndef EXTERNINTERFACEPARSER_H
#define EXTERNINTERFACEPARSER_H

#pragma once

#include "AST/TypeDeclNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // declaration pass: mint the private slots and `$__image`, default
    // abi_link_name. bodies wait until pass 3 - parse_funcdecl replace_args
    // in the body pass would otherwise leave the forwarder's `$this` pointing
    // at a parameter the function no longer has
    void finalize_extern_interface(
        Payload &payload,
        AST::TypeDeclNode *type
    );

    // pass 3: give each requirement a forwarder body against the args that
    // survived replace_args
    void plant_extern_interface_bodies(
        Payload &payload,
        AST::TypeDeclNode *type
    );
};

#endif
