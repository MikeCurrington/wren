#pragma once
#ifndef WREN_FOREIGN_CLASS_H
#define WREN_FOREIGN_CLASS_H

#include "wren.h"

// Registers the "foreign_classes" module for test/api/foreign_class.wren via
// the C++20 bindings API. Must run before the test script is interpreted.
void foreignClassSetup(WrenVM* vm);

#endif
