// What msvcrt.cpp shares with the rest of the msvcrt.dll HLE (its printf
// functions, in ucrt_stdio.cpp).

#pragma once

#include <cstdio>

#include "runtime/guest_memory.h"

namespace recomp::hle {

// The host stream behind a guest FILE* (one of _iob's, or one fopen made);
// fatal for an unknown one, naming api.
std::FILE* MsvcrtFile(gaddr fp, const char* api);

}  // namespace recomp::hle
