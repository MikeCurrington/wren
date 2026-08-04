#ifndef CPP20_BINDINGS_H
#define CPP20_BINDINGS_H

#include "wren.h"

// Main test runner for foreign binding tests
int cpp20BindingsRunTests(WrenVM* vm);

// Foreign method binding function
WrenForeignMethodFn cpp20BindingsBindMethod(const char* signature);

// Foreign class binding function
void cpp20BindingsBindClass(const char* className, WrenForeignClassMethods* methods);

#endif // CPP20_BINDINGS_H
