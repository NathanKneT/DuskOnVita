#pragma once
// Force-included first in every game TU (via the precompiled header).
// newlib's <ctype.h> defines the classification masks _U _L _N _S _P _C _X _B
// as macros (__MISC_VISIBLE). The decomp uses _C as a field name (offset-named
// member in include/d/d_event_lib.h), which then expands to 040. libstdc++'s
// ctype_base needs the masks, so parse it here first and drop only _C after.
#ifdef __cplusplus
#include <cctype>
#include <locale>
#undef _C
#endif
