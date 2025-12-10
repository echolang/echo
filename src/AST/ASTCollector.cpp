#include "AST/ASTCollector.h"
#include "AST/ASTDiagnosticRenderer.h"

#include <algorithm>

AST::Collector::Collector()
{

}

AST::Collector::~Collector()
{
}

void AST::Collector::print_issues(const AST::DiagnosticRenderer &renderer) const
{
    // **insertion order, deliberately.** that is pass order, and pass order is causal - a parse error
    // precedes the type errors it caused, and sorting by location would put the consequence above the
    // cause. An editor sorts for itself and does not need this to
    for (const auto &issue : issues) {
        renderer.render_issue(*issue);
    }
}

bool AST::Collector::has_critical_issues() const
{
    return std::any_of(issues.begin(), issues.end(), [](const auto &issue) {
        return issue->is_critical();
    });
}

size_t AST::Collector::count_of(AST::IssueSeverity severity) const
{
    return static_cast<size_t>(std::count_if(issues.begin(), issues.end(), [severity](const auto &issue) {
        return issue->severity == severity;
    }));
}
