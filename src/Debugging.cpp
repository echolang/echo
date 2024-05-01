#include "Debugging.h"

std::string DD::tabbify(const std::string& str, size_t n, char c) 
{
    std::string result;
    bool nl = true;

    for (char const &ch : str) {
        if (nl) {
            result.append(n, c);
            nl = false;
        }

        result.push_back(ch);

        if (ch == '\n') {
            nl = true;
        }
    }

    return result;
}