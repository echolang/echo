#include <iostream>
#include <fstream>
#include <sstream>

#include "Lexer.h"

int main() {
    Lexer lexer;
    TokenCollection tokens;

    // load the file "test.eco" and convert it to a string
    std::ifstream file("test.eco");
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string str = buffer.str();

    lexer.tokenize(tokens, str);

    // print tokens
    for (size_t i = 0; i < tokens.tokens.size(); i++) {
        std::cout << "Token: " << tokens.token_values[i] << " Type: " << static_cast<int>(tokens.tokens[i].type) << " Line: " << tokens.tokens[i].line << " Char: " << tokens.tokens[i].char_offset << std::endl;
    }

    return 0;
}
