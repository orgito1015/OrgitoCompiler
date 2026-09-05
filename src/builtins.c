#include "builtins.h"

const NativeSig NATIVE_SIGS[NATIVE_COUNT] = {
    { "print",  -1 },
    { "malloc",  1 },
    { "free",    1 },
    { "strlen",  1 },
    { "strcmp",  2 },
    { "strcpy",  2 },
};
