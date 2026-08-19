#include <stdio.h>
#include <string.h>

#include "wren.h"

int fiberExitRunTests(WrenVM* vm)
{
  wrenEnsureSlots(vm, 1);
  wrenGetVariable(vm, "./test/api/fiber_exit", "Test", 0);
  WrenHandle* testClass = wrenGetSlotHandle(vm, 0);

  WrenHandle* exitWithValue = wrenMakeCallHandle(vm, "exitWithValue()");
  WrenHandle* exitWithNull = wrenMakeCallHandle(vm, "exitWithNull()");

  // A fiber that exits with a value should make wrenCall() succeed and expose
  // the exit value in slot 0.
  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, testClass);
  WrenInterpretResult result = wrenCall(vm, exitWithValue);
  printf("result: %s\n", result == WREN_RESULT_SUCCESS ? "success" : "error");
  printf("value: %s\n", wrenGetSlotString(vm, 0));

  // A fiber that exits with null should make wrenCall() succeed and expose
  // null in slot 0.
  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, testClass);
  result = wrenCall(vm, exitWithNull);
  printf("result: %s\n", result == WREN_RESULT_SUCCESS ? "success" : "error");
  printf("type: %d\n", wrenGetSlotType(vm, 0));

  wrenReleaseHandle(vm, testClass);
  wrenReleaseHandle(vm, exitWithValue);
  wrenReleaseHandle(vm, exitWithNull);
  return 0;
}
