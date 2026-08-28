#include <stdio.h>
#include <string.h>

#include "debug_hook.h"

// The module the .wren side of this test runs in. The hook filters on it so
// that any events from core or optional module code don't pollute the output.
static const char* testModule = "./test/api/debug_hook";

static void debugHook(WrenVM* vm, WrenDebugEvent event, void* userData)
{
  WrenDebugFrameInfo info;
  wrenDebugGetFrameInfo(vm, 0, &info);
  if (strcmp(info.module, testModule) != 0) return;

  wrenEnsureSlots(vm, 2);
  printf("line %d in %s, %d frame(s)\n", info.line, info.function,
         wrenDebugGetFrameCount(vm));

  // When paused at the top of the test method, inspect its locals and the
  // module variables of its module.
  if (strncmp(info.function, "bar", 3) == 0)
  {
    printf("bar has %d local(s)\n", wrenDebugGetLocalCount(vm, 0));

    wrenDebugGetLocal(vm, 0, 1, 0);
    printf("bar's first local slot after the receiver is %g\n",
           wrenGetSlotDouble(vm, 0));

    int numVariables = wrenDebugGetModuleVariableCount(vm, 0);
    for (int i = 0; i < numVariables; i++)
    {
      const char* name = wrenDebugGetModuleVariableName(vm, 0, i);
      if (strcmp(name, "x") == 0)
      {
        wrenDebugGetModuleVariable(vm, 0, i, 0);
        printf("module variable x is %g\n", wrenGetSlotDouble(vm, 0));
      }
    }
  }
}

static void install(WrenVM* vm)
{
  wrenSetDebugHook(vm, debugHook, NULL);
}

static void uninstall(WrenVM* vm)
{
  wrenSetDebugHook(vm, NULL, NULL);
}

WrenForeignMethodFn debugHookBindMethod(const char* signature)
{
  if (strcmp(signature, "static Debug.install()") == 0) return install;
  if (strcmp(signature, "static Debug.uninstall()") == 0) return uninstall;

  return NULL;
}
