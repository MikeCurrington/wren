#include "wren.h"

int callNestedRunTests(WrenVM* vm);

WrenForeignMethodFn callNestedBindMethod(const char* signature);

void callNestedBindClass(const char* className,
                         WrenForeignClassMethods* methods);

