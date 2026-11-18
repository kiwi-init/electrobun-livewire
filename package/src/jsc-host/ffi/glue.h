// The generated bun:ffi glue (ffi_glue.c): installs globalThis.__ffi. Not
// ffi.h: that name is libffi's (dynamic.inc includes it).
#pragma once
#include "../runtime.h"

void ffi_install(jsr_runtime *rt);
