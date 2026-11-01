#include "AST/ASTLayoutQuery.h"

namespace
{
    AST::LayoutQueryFn t_alloc_size = nullptr;
    AST::LayoutQueryFn t_abi_align = nullptr;
}

AST::LayoutQueryScope::LayoutQueryScope(LayoutQueryFn alloc_size, LayoutQueryFn abi_align)
    : _previous_size(t_alloc_size), _previous_align(t_abi_align)
{
    t_alloc_size = alloc_size;
    t_abi_align = abi_align;
}

AST::LayoutQueryScope::~LayoutQueryScope()
{
    t_alloc_size = _previous_size;
    t_abi_align = _previous_align;
}

std::optional<uint64_t> AST::layout_alloc_size(const ValueType &type)
{
    if (t_alloc_size == nullptr) {
        return std::nullopt;
    }

    return t_alloc_size(type);
}

std::optional<uint64_t> AST::layout_abi_align(const ValueType &type)
{
    if (t_abi_align == nullptr) {
        return std::nullopt;
    }

    return t_abi_align(type);
}
