#include "wren.h"

WrenForeignMethodFn foreignStackMoveBindMethod(const char* fullName);
void foreignStackMoveBindClass(const char* className,
                               WrenForeignClassMethods* methods);
int foreignStackMoveRunTests(WrenVM* vm);
