// Natives behind the Node shims: installs globalThis.__node.
#pragma once
#include "runtime.h"

void node_install(jsr_runtime *rt);
