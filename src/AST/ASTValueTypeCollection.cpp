#include "AST/ASTValueTypeCollection.h"

AST::ValueTypeCollection::ValueTypeCollection()
{

}

AST::ValueTypeCollection::~ValueTypeCollection()
{
}

AST::vt_handle_t AST::ValueTypeCollection::push_type(ValueType type)
{
    value_types.push_back(type);
    return value_types.size() - 1;
}