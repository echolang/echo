#ifndef ASTCOLLECTOR_H
#define ASTCOLLECTOR_H

#pragma once

#include "AST/ASTValueTypeCollection.h"
#include "AST/ASTIssue.h"
#include "AST/ASTContext.h"

namespace AST
{  
    class Collector
    {
    public:
        ValueTypeCollection value_types;
        std::vector<Issue> issues;
        

        Collector();
        ~Collector();

        void collect_issue(const Context &context, Issue &issue);
    };
};

#endif