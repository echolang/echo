#if defined(__SSE2__)
#include <emmintrin.h>
#endif

int eco_sse_add(int a, int b)
{
#if defined(__SSE2__)
    /* `_mm_set1_epi32` / `_mm_add_epi32` are the names that came out as
       undefined symbols when MSVC's emmintrin.h won */
    __m128i x = _mm_set1_epi32(a);
    __m128i y = _mm_set1_epi32(b);
    __m128i s = _mm_add_epi32(x, y);
    return _mm_cvtsi128_si32(s);
#else
    return a + b;
#endif
}
