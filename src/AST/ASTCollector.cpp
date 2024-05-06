#include "AST/ASTCollector.h"

#include <iostream>

AST::Collector::Collector()
{

}

AST::Collector::~Collector()
{
}

void AST::Collector::print_issues() const
{
    for (const auto &issue : issues)
    {
        std::cout << "---- Issue ----" << std::endl;
        std::cout << "Issue at " << issue->code_ref.token_slice.startt().line << ":" << issue->code_ref.token_slice.startt().char_offset << std::endl;
        std::cout << issue->message() << std::endl;
        std::cout << issue->code_ref.get_referenced_code_excerpt() << std::endl;
    }
}