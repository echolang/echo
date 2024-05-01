#ifndef LEXER_H
#define LEXER_H

#pragma once

#include <string>

namespace DD
{
    std::string tabbify(const std::string &str, size_t n, char c = ' ');
}

#endif