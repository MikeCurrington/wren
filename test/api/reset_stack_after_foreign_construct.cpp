#include <stdio.h>
#include <string.h>
#include <memory>

#include "wren.h"
#include "wren_vm.h"

static void counterAllocate(WrenVM* vm)
{
  vm->SetSlotNewForeignShared(0, 0,
      std::shared_ptr<double>(new double(wrenGetSlotDouble(vm, 1)),
          std::default_delete<double>()), nullptr);
}

void resetStackAfterForeignConstructBindClass(
    const char* className, WrenForeignClassMethods* methods)
{
  if (strcmp(className, "ResetStackForeign") == 0)
  {
    methods->allocate = counterAllocate;
    return;
  }
}

int resetStackAfterForeignConstructRunTests(WrenVM* vm)
{
  wrenEnsureSlots(vm, 1);
  wrenGetVariable(vm,
      "./test/api/reset_stack_after_foreign_construct", "Test", 0);
  WrenHandle* testClass = wrenGetSlotHandle(vm, 0);

  WrenHandle* callConstruct = wrenMakeCallHandle(vm, "callConstruct()");
  WrenHandle* afterConstruct = wrenMakeCallHandle(vm, "afterConstruct(_,_)");

  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, testClass);
  wrenCall(vm, callConstruct);

  wrenEnsureSlots(vm, 3);
  wrenSetSlotHandle(vm, 0, testClass);
  wrenSetSlotDouble(vm, 1, 1.0);
  wrenSetSlotDouble(vm, 2, 2.0);
  wrenCall(vm, afterConstruct);

  wrenReleaseHandle(vm, testClass);
  wrenReleaseHandle(vm, callConstruct);
  wrenReleaseHandle(vm, afterConstruct);

  return 0;
}
