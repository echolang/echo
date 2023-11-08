#include "Token.h"

TokenReference TokenCollection::operator[](size_t index) const
{
    return TokenReference(*this, index);
}
