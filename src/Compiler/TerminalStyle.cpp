#include "Compiler/TerminalStyle.h"

std::string Compiler::styled(const std::string &text, const char *sgr, bool color)
{
    if (!color) {
        return text;
    }

    return std::string(sgr) + text + sgr::reset;
}
