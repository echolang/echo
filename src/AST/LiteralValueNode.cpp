#include "AST/LiteralValueNode.h"

const std::string &AST::LiteralStringExprNode::get_string_value() const
{
    // the decoded bytes, not the token's interior. one answer for every reader - the `#[builtin: "..."]`
    // attribute readers, the abort message ExprCodegen folds, and the constant codegen emits - so a
    // literal can never mean one thing in a signature and another in the binary
    return decoded_value;
}
