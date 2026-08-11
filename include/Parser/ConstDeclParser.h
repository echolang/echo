#ifndef CONSTDECLPARSER_H
#define CONSTDECLPARSER_H

#pragma once

#include "AST/ConstDeclNode.h"
#include "Parser/ParserPayload.h"
#include "Parser/VisibilityParser.h"

namespace AST
{
    class TypeDeclNode;
};

namespace Parser
{
    // reads one **compile-time constant** declaration - `const usize MAX = 100;` - and publishes its name
    // as a namespace symbol. `owner` is the struct or class whose body it was written in, null at file or
    // namespace scope; a struct's constants are published into its member surface, so `buffer::MAX` and
    // `buffer::Inner(1)` resolve through the same path.
    //
    // **called from the declaration pass only.** That pass walks every file before any body is parsed, which
    // is what makes a constant nameable from anywhere in the program regardless of file order; the body pass
    // skips the statement, and the dispatch site there says so. The initializer is parsed *here*, in the
    // declaration pass, exactly as a struct property's is - the references inside it are resolved later, by
    // AST::ConstantExpander, so nothing depends on which file this one happened to sit in.
    //
    // null when the declaration was refused, in which case nothing has been published and the cursor has
    // been moved past the statement.
    //
    // `visibility` is what the dispatch that reached this already consumed. A **member** constant takes
    // none: it is reached through its owner's member surface, so a modifier there would be the member axis,
    // and this refuses one rather than silently reading it as the file axis
    AST::ConstDeclNode *parse_constdecl(
        Parser::Payload &payload,
        AST::TypeDeclNode *owner,
        VisibilityPrefix visibility
    );
};

#endif
