#pragma once
/* Port-wide settings of the Amiga port. */
#include "../types.h"

/* --original-bugs: behave exactly like the original instead of applying the port's fixes
 * (port/amiga/README.md, "Original bugs"). Each fix tests it at the place its spec describes. */
extern bool g_original_bugs;
