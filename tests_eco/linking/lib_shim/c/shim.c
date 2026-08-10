#include "shim.h"

/* ECO_SHIM_BASE comes from the manifest's `define:`, ECO_SHIM_BONUS from the header the
   `include:` path made reachable - so 42 is both of them arriving */
int eco_shim_answer(void)
{
    return ECO_SHIM_BASE + ECO_SHIM_BONUS;
}
