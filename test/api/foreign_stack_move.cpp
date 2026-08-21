#include <stdio.h>
#include <string.h>

#include "wren.h"

// Mimics the "raw" binding style used by pinscript: an instance getter that
// returns a newly created foreign value, fetching the Wren class into a
// scratch slot above the current arguments. That forces wrenEnsureSlots() to
// grow (and usually reallocate) the fiber's value stack from inside the
// foreign call. See foreign_stack_move.wren for the bug this guards against.

static void foreignStackMoveBoardAllocate(WrenVM* vm)
{
  double* payload = (double*)wrenSetSlotNewForeign(vm, 0, 0, sizeof(double));
  *payload = wrenGetSlotDouble(vm, 1);
}

static void foreignStackMoveSignalAllocate(WrenVM* vm)
{
  double* payload = (double*)wrenSetSlotNewForeign(vm, 0, 0, sizeof(double));
  *payload = wrenGetSlotDouble(vm, 1);
}

static void foreignStackMoveSignalValueGet(WrenVM* vm)
{
  double* payload = (double*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, *payload);
}

static void foreignStackMoveBoardSignalGet(WrenVM* vm)
{
  double* payload = (double*)wrenGetSlotForeign(vm, 0);
  // Place the class in a scratch slot past the call's arguments (slot 0
  // holds the receiver), exactly like pinscript's RawMemberVarGet.
  const int slots = wrenGetSlotCount(vm);
  const int classSlot = slots < 8 ? 8 : slots;
  wrenEnsureSlots(vm, classSlot + 1);
  wrenGetVariable(vm, "./test/api/foreign_stack_move", "Signal", classSlot);
  double* signal =
      (double*)wrenSetSlotNewForeign(vm, 0, classSlot, sizeof(double));
  *signal = *payload;
}

WrenForeignMethodFn foreignStackMoveBindMethod(const char* fullName)
{
  if (strcmp(fullName, "Board.signal") == 0)
    return foreignStackMoveBoardSignalGet;
  if (strcmp(fullName, "Signal.value") == 0)
    return foreignStackMoveSignalValueGet;
  return NULL;
}

void foreignStackMoveBindClass(const char* className,
                               WrenForeignClassMethods* methods)
{
  if (strcmp(className, "Board") == 0)
  {
    methods->allocate = foreignStackMoveBoardAllocate;
    return;
  }
  if (strcmp(className, "Signal") == 0)
  {
    methods->allocate = foreignStackMoveSignalAllocate;
    return;
  }
}

int foreignStackMoveRunTests(WrenVM* vm)
{
  wrenEnsureSlots(vm, 1);
  wrenGetVariable(vm, "./test/api/foreign_stack_move", "Test", 0);
  WrenHandle* testClass = wrenGetSlotHandle(vm, 0);

  WrenHandle* mapped = wrenMakeCallHandle(vm, "mapped()");
  WrenHandle* mappedInFiber = wrenMakeCallHandle(vm, "mappedInFiber()");
  WrenHandle* manual = wrenMakeCallHandle(vm, "manual()");

  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, testClass);
  wrenCall(vm, mapped);

  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, testClass);
  wrenCall(vm, mappedInFiber);

  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, testClass);
  wrenCall(vm, manual);

  wrenReleaseHandle(vm, testClass);
  wrenReleaseHandle(vm, mapped);
  wrenReleaseHandle(vm, mappedInFiber);
  wrenReleaseHandle(vm, manual);
  return 0;
}
