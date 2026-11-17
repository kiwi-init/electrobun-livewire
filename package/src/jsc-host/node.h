// Natives behind the Node shims: installs globalThis.__node.
#pragma once
#include "runtime.h"

void node_install(jsr_runtime *rt);
// bun:sqlite's natives (sqlite.c): globalThis.__sqlite.
void sqlite_install(jsr_runtime *rt);
