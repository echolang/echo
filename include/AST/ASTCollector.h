#ifndef ASTCOLLECTOR_H
#define ASTCOLLECTOR_H

#pragma once

#include "AST/ASTValueTypeCollection.h"
#include "AST/ASTIssue.h"
#include "AST/ASTContext.h"
#include "AST/ASTOps.h"

namespace AST
{  
    class Collector
    {
    public:
        std::vector<std::unique_ptr<AST::IssueRecord>> issues;
        ValueTypeCollection value_types = ValueTypeCollection();
        OperatorRegistry operators = OperatorRegistry();
        
        Collector();
        ~Collector();

        template <typename T, typename... Args>
        void collect_issue(const CodeRef &code_ref, Args... args) {
            issues.push_back(std::make_unique<T>(code_ref, args...));
        }

        void print_issues() const;

        bool has_critical_issues() const;
    };
};

#endif