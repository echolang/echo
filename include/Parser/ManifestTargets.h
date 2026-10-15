#ifndef MANIFESTTARGETS_H
#define MANIFESTTARGETS_H

#pragma once

#include "AST/ASTAttributeReader.h"
#include "AST/ASTAttributeValue.h"
#include "Parser/ManifestScope.h"

#include <string>
#include <vector>

namespace Parser
{
    // a target as written, before `#[sources:]` has been expanded to check its entry against
    struct WrittenTarget
    {
        WrittenScope scope;
        std::string name;
        std::string entry;
        TargetKind kind = TargetKind::t_executable;
        bool install = false;
        bool is_public = false;
        std::vector<std::string> groups;
        std::vector<std::string> files;
        uint32_t line = 0;
        bool has_scope = false;
    };

    // `#[target: ...]`, in every shape it takes. the tag is the kind and the record is the fields,
    // the shape `#[hook: before_build { ... }]` has
    void read_manifest_targets(
        const AST::AttributeValue &written,
        AST::AttributeReader &reader,
        uint32_t line,
        std::vector<WrittenTarget> &out_targets
    );
};

#endif
