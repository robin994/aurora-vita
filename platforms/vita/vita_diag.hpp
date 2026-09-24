#pragma once

#include <cstdio>

#if defined(AURORA_VITA_NO_DIAGNOSTICS)
#define AURORA_VITA_DIAGF(...) ((void)0)
#else
#define AURORA_VITA_DIAGF(...) std::fprintf(stderr, __VA_ARGS__)
#endif
