#include <stdio.h>
#include <string.h>

#include "wren.h"

// Tests for re-entrant calls into the VM from inside foreign methods, using
// wrenBeginCall() and wrenEndCall(). See call_nested.wren for the Wren side
// and the expected output of each scenario.

// Handles used by the foreign methods to call back into Wren. Created lazily
// on first use and released by callNestedCleanup() at the end of the test.
static WrenHandle* nestedClass = NULL;
static WrenHandle* addMethod = NULL;
static WrenHandle* multiplyMethod = NULL;
static WrenHandle* doubleMethod = NULL;
static WrenHandle* throwerMethod = NULL;
static WrenHandle* exitValueMethod = NULL;
static WrenHandle* allocateMethod = NULL;
static WrenHandle* stackGrowMethod = NULL;
static WrenHandle* level1Method = NULL;
static WrenHandle* suspendMethod = NULL;
static WrenHandle* attemptCallMethod = NULL;
static WrenHandle* attemptTransferMethod = NULL;

static void ensureHandles(WrenVM* vm)
{
  if (nestedClass != NULL) return;

  // This clobbers slot 0 (the receiver), so foreign methods that need their
  // own arguments call this before reading them.
  wrenEnsureSlots(vm, 1);
  wrenGetVariable(vm, "./test/api/call_nested", "Nested", 0);
  nestedClass = wrenGetSlotHandle(vm, 0);

  addMethod = wrenMakeCallHandle(vm, "add(_,_)");
  multiplyMethod = wrenMakeCallHandle(vm, "multiply(_,_)");
  doubleMethod = wrenMakeCallHandle(vm, "double(_)");
  throwerMethod = wrenMakeCallHandle(vm, "thrower()");
  exitValueMethod = wrenMakeCallHandle(vm, "exitValue()");
  allocateMethod = wrenMakeCallHandle(vm, "allocate(_)");
  stackGrowMethod = wrenMakeCallHandle(vm, "stackGrow(_)");
  level1Method = wrenMakeCallHandle(vm, "level1(_)");
  suspendMethod = wrenMakeCallHandle(vm, "suspend()");
  attemptCallMethod = wrenMakeCallHandle(vm, "attemptCall(_)");
  attemptTransferMethod = wrenMakeCallHandle(vm, "attemptTransfer(_)");
}

static void callNestedCleanup(WrenVM* vm)
{
  if (nestedClass == NULL) return;
  wrenReleaseHandle(vm, nestedClass);
  wrenReleaseHandle(vm, addMethod);
  wrenReleaseHandle(vm, multiplyMethod);
  wrenReleaseHandle(vm, doubleMethod);
  wrenReleaseHandle(vm, throwerMethod);
  wrenReleaseHandle(vm, exitValueMethod);
  wrenReleaseHandle(vm, allocateMethod);
  wrenReleaseHandle(vm, stackGrowMethod);
  wrenReleaseHandle(vm, level1Method);
  wrenReleaseHandle(vm, suspendMethod);
  wrenReleaseHandle(vm, attemptCallMethod);
  wrenReleaseHandle(vm, attemptTransferMethod);
  nestedClass = NULL;
}

// The basic round trip: hide the foreign method's slots, call
// Nested.add(_,_) on a fresh fiber, restore the slots, and return the result.
static void nestedCallWren(WrenVM* vm)
{
  ensureHandles(vm);
  double a = wrenGetSlotDouble(vm, 1);
  double b = wrenGetSlotDouble(vm, 2);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 3);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenSetSlotDouble(vm, 1, a);
  wrenSetSlotDouble(vm, 2, b);
  WrenInterpretResult result = wrenCall(vm, addMethod);
  double sum = (result == WREN_RESULT_SUCCESS)
      ? wrenGetSlotDouble(vm, 0) : -1.0;
  wrenEndCall(vm, context);

  printf("own args after nested call: %g %g\n", a, b);
  wrenSetSlotDouble(vm, 0, sum);
}

// wrenInterpret() from inside a foreign method. It needs no wrenBeginCall()
// because it doesn't use the API slots, but it must still preserve them.
static void nestedInterpret(WrenVM* vm)
{
  const char* arg = wrenGetSlotString(vm, 1);

  wrenInterpret(vm, "./test/api/call_nested_interpret_module",
      "System.print(\"[nested interpret]\")");

  printf("own arg after nested interpret: %s\n", wrenGetSlotString(vm, 1));
  wrenSetSlotString(vm, 0, arg);
}

// A foreign method that forgets to call wrenEndCall(). The VM should clean
// up the leaked context when the foreign method returns. Since the method
// never writes its return slot, it returns its receiver (the Nested class).
static void nestedForgetEnd(WrenVM* vm)
{
  ensureHandles(vm);
  double a = wrenGetSlotDouble(vm, 1);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 2);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenSetSlotDouble(vm, 1, a);
  wrenCall(vm, doubleMethod);

  // Intentionally "forget" wrenEndCall() here to test that the VM cleans up
  // the leaked context when the foreign method returns.
  (void)context;
}

// Two sequential begin/call/end cycles within one foreign method.
static void nestedTwoCalls(WrenVM* vm)
{
  ensureHandles(vm);
  double a = wrenGetSlotDouble(vm, 1);
  double b = wrenGetSlotDouble(vm, 2);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 3);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenSetSlotDouble(vm, 1, a);
  wrenSetSlotDouble(vm, 2, b);
  WrenInterpretResult result = wrenCall(vm, addMethod);
  double sum = (result == WREN_RESULT_SUCCESS)
      ? wrenGetSlotDouble(vm, 0) : -1.0;

  // The previous call's slots are reused (and reset) for the second call.
  wrenEnsureSlots(vm, 3);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenSetSlotDouble(vm, 1, a);
  wrenSetSlotDouble(vm, 2, b);
  result = wrenCall(vm, multiplyMethod);
  double product = (result == WREN_RESULT_SUCCESS)
      ? wrenGetSlotDouble(vm, 0) : -1.0;
  wrenEndCall(vm, context);

  printf("results: %g %g\n", sum, product);
  wrenSetSlotDouble(vm, 0, product);
}

// Full C-stack nesting: this foreign method calls Wren, whose nested code
// calls another foreign method that calls Wren again.
static void nestedLevel0(WrenVM* vm)
{
  ensureHandles(vm);
  double a = wrenGetSlotDouble(vm, 1);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 2);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenSetSlotDouble(vm, 1, a);
  WrenInterpretResult result = wrenCall(vm, level1Method);
  double value = (result == WREN_RESULT_SUCCESS)
      ? wrenGetSlotDouble(vm, 0) : -1.0;
  wrenEndCall(vm, context);

  wrenSetSlotDouble(vm, 0, value);
}

// The inner level of the nesting: called from Nested.level1(), which itself
// was called from nestedLevel0()'s nested wrenCall().
static void nestedLevel1Foreign(WrenVM* vm)
{
  ensureHandles(vm);
  double x = wrenGetSlotDouble(vm, 1);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 3);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenSetSlotDouble(vm, 1, x);
  wrenSetSlotDouble(vm, 2, 2.0);
  WrenInterpretResult result = wrenCall(vm, multiplyMethod);
  double product = (result == WREN_RESULT_SUCCESS)
      ? wrenGetSlotDouble(vm, 0) : -1.0;
  wrenEndCall(vm, context);

  wrenSetSlotDouble(vm, 0, product);
}

// The nested code allocates heavily and forces a GC. The suspended fiber's
// stack (which holds the foreign method's arguments and its Wren caller's
// locals) must survive the collection.
static void nestedGcStress(WrenVM* vm)
{
  ensureHandles(vm);
  const char* arg = wrenGetSlotString(vm, 1);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 2);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenSetSlotDouble(vm, 1, 100);
  WrenInterpretResult result = wrenCall(vm, allocateMethod);
  double count = (result == WREN_RESULT_SUCCESS)
      ? wrenGetSlotDouble(vm, 0) : -1.0;
  wrenEndCall(vm, context);

  printf("nested allocated %g\n", count);
  printf("own arg after gc: %s\n", arg);
  wrenSetSlotDouble(vm, 0, 9);
}

// Forces a full collection from inside a foreign method of the nested call.
static void nestedGc(WrenVM* vm)
{
  wrenCollectGarbage(vm);
}

// Grows the API slots of the nested call far beyond the fresh fiber's initial
// stack capacity, forcing the stack to be reallocated (moved).
static void nestedSlots(WrenVM* vm)
{
  int n = (int)wrenGetSlotDouble(vm, 1);
  wrenEnsureSlots(vm, n);
  printf("nested slots %d\n", wrenGetSlotCount(vm));
  wrenSetSlotDouble(vm, 0, 8);
}

// Like nestedCallWren(), but the nested call grows and moves the nested
// fiber's stack. The foreign method's own slots must not be disturbed.
static void nestedStackMove(WrenVM* vm)
{
  ensureHandles(vm);
  const char* arg = wrenGetSlotString(vm, 1);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 2);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenSetSlotDouble(vm, 1, 60);
  WrenInterpretResult result = wrenCall(vm, stackGrowMethod);
  double value = (result == WREN_RESULT_SUCCESS)
      ? wrenGetSlotDouble(vm, 0) : -1.0;
  wrenEndCall(vm, context);

  printf("own arg after move: %s\n", arg);
  wrenSetSlotDouble(vm, 0, value);
}

// The nested call aborts with a runtime error. The C code observes the error
// result, restores its own slots, and returns normally.
static void nestedErrorHandled(WrenVM* vm)
{
  ensureHandles(vm);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, nestedClass);
  WrenInterpretResult result = wrenCall(vm, throwerMethod);
  wrenEndCall(vm, context);

  if (result != WREN_RESULT_RUNTIME_ERROR)
  {
    printf("nested call should have failed\n");
  }
  else
  {
    printf("nested call failed as expected\n");
  }

  wrenSetSlotString(vm, 0, "handled");
}

// The nested call aborts and the C code propagates the failure outward by
// aborting its own fiber.
static void nestedErrorPropagate(WrenVM* vm)
{
  ensureHandles(vm);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenCall(vm, throwerMethod);
  wrenEndCall(vm, context);

  wrenSetSlotString(vm, 0, "nested error");
  wrenAbortFiber(vm, 0);
}

// The nested code exits its fiber with a value using Fiber.exit(). The nested
// wrenCall() should succeed and expose the exit value in slot 0.
static void nestedFiberExit(WrenVM* vm)
{
  ensureHandles(vm);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, nestedClass);
  WrenInterpretResult result = wrenCall(vm, exitValueMethod);
  int code = (result == WREN_RESULT_SUCCESS) ? 0 : 1;

  // Hold on to the result with a handle so it can't be collected between
  // ending the nested call and writing the return slot.
  WrenHandle* value = (result == WREN_RESULT_SUCCESS)
      ? wrenGetSlotHandle(vm, 0) : NULL;
  wrenEndCall(vm, context);

  printf("nested exit result: %d\n", code);
  if (value != NULL)
  {
    wrenSetSlotHandle(vm, 0, value);
    wrenReleaseHandle(vm, value);
  }
  else
  {
    wrenSetSlotString(vm, 0, "failed");
  }
}

// The nested code suspends its fiber with Fiber.yield(). Only the innermost
// interpreter stops; wrenCall() reports success but there is no return value,
// so the foreign method must not read its slots until wrenEndCall() restores
// its own.
static void nestedSuspend(WrenVM* vm)
{
  ensureHandles(vm);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, nestedClass);
  WrenInterpretResult result = wrenCall(vm, suspendMethod);
  wrenEndCall(vm, context);

  wrenSetSlotString(vm, 0,
      result == WREN_RESULT_SUCCESS ? "nested suspended" : "failed");
}

// The nested code tries to call [fiber], which is suspended in this foreign
// method. It must get a runtime error instead of resuming the fiber, which
// aborts the nested call. The C code observes the error and reports it.
static void nestedGuardCall(WrenVM* vm)
{
  ensureHandles(vm);
  WrenHandle* fiberHandle = wrenGetSlotHandle(vm, 1);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 2);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenSetSlotHandle(vm, 1, fiberHandle);
  WrenInterpretResult result = wrenCall(vm, attemptCallMethod);
  wrenEndCall(vm, context);

  wrenSetSlotString(vm, 0,
      result == WREN_RESULT_RUNTIME_ERROR ? "call blocked" : "call NOT blocked");
  wrenReleaseHandle(vm, fiberHandle);
}

// Like nestedGuardCall(), but using transfer instead of call.
static void nestedGuardTransfer(WrenVM* vm)
{
  ensureHandles(vm);
  WrenHandle* fiberHandle = wrenGetSlotHandle(vm, 1);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 2);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenSetSlotHandle(vm, 1, fiberHandle);
  WrenInterpretResult result = wrenCall(vm, attemptTransferMethod);
  wrenEndCall(vm, context);

  wrenSetSlotString(vm, 0,
      result == WREN_RESULT_RUNTIME_ERROR ? "transfer blocked"
                                          : "transfer NOT blocked");
  wrenReleaseHandle(vm, fiberHandle);
}

// The allocator for the foreign NestedForeign class. It calls back into Wren
// from inside createForeign(), exercising nesting in a foreign constructor.
static void nestedForeignAllocate(WrenVM* vm)
{
  ensureHandles(vm);
  double x = wrenGetSlotDouble(vm, 1);

  WrenCallContext* context = wrenBeginCall(vm);
  wrenEnsureSlots(vm, 2);
  wrenSetSlotHandle(vm, 0, nestedClass);
  wrenSetSlotDouble(vm, 1, x);
  WrenInterpretResult result = wrenCall(vm, doubleMethod);
  double value = (result == WREN_RESULT_SUCCESS)
      ? wrenGetSlotDouble(vm, 0) : -1.0;
  wrenEndCall(vm, context);

  double* data = (double*)wrenSetSlotNewForeign(vm, 0, 0, sizeof(double));
  *data = value;
}

static void nestedForeignFinalize(void* data)
{
  // Nothing to clean up; the finalizer exists to match the class's shape.
  (void)data;
}

static void nestedForeignValue(WrenVM* vm)
{
  double* data = (double*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, *data);
}

void callNestedBindClass(const char* className, WrenForeignClassMethods* methods)
{
  if (strcmp(className, "NestedForeign") == 0)
  {
    methods->allocate = nestedForeignAllocate;
    methods->finalize = nestedForeignFinalize;
  }
}

WrenForeignMethodFn callNestedBindMethod(const char* signature)
{
  if (strcmp(signature, "static Nested.callWren(_,_)") == 0) return nestedCallWren;
  if (strcmp(signature, "static Nested.interpret(_)") == 0) return nestedInterpret;
  if (strcmp(signature, "static Nested.forgetEnd(_)") == 0) return nestedForgetEnd;
  if (strcmp(signature, "static Nested.twoCalls(_,_)") == 0) return nestedTwoCalls;
  if (strcmp(signature, "static Nested.level0(_)") == 0) return nestedLevel0;
  if (strcmp(signature, "static Nested.gcStress(_)") == 0) return nestedGcStress;
  if (strcmp(signature, "static Nested.stackMove(_)") == 0) return nestedStackMove;
  if (strcmp(signature, "static Nested.errorHandled()") == 0) return nestedErrorHandled;
  if (strcmp(signature, "static Nested.errorPropagate()") == 0) return nestedErrorPropagate;
  if (strcmp(signature, "static Nested.fiberExit()") == 0) return nestedFiberExit;
  if (strcmp(signature, "static Nested.nestedSuspend()") == 0) return nestedSuspend;
  if (strcmp(signature, "static Nested.guardCall(_)") == 0) return nestedGuardCall;
  if (strcmp(signature, "static Nested.guardTransfer(_)") == 0) return nestedGuardTransfer;
  if (strcmp(signature, "static Nested.gc()") == 0) return nestedGc;
  if (strcmp(signature, "static Nested.slots(_)") == 0) return nestedSlots;
  if (strcmp(signature, "static Nested.level1Foreign(_)") == 0) return nestedLevel1Foreign;
  if (strcmp(signature, "NestedForeign.value") == 0) return nestedForeignValue;

  return NULL;
}

int callNestedRunTests(WrenVM* vm)
{
  int exitCode = 0;

  wrenEnsureSlots(vm, 1);
  wrenGetVariable(vm, "./test/api/call_nested", "Nested", 0);
  WrenHandle* nested = wrenGetSlotHandle(vm, 0);
  WrenHandle* runAll = wrenMakeCallHandle(vm, "runAll()");
  WrenHandle* runErrorPropagate = wrenMakeCallHandle(vm, "runErrorPropagate()");

  // Run all of the scenarios that complete normally.
  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, nested);
  WrenInterpretResult result = wrenCall(vm, runAll);
  if (result != WREN_RESULT_SUCCESS) exitCode = 1;

  // The propagation scenario aborts the fiber, so it runs separately.
  wrenEnsureSlots(vm, 1);
  wrenSetSlotHandle(vm, 0, nested);
  result = wrenCall(vm, runErrorPropagate);
  if (result != WREN_RESULT_RUNTIME_ERROR)
  {
    printf("Missing propagated runtime error.\n");
    exitCode = 1;
  }

  wrenReleaseHandle(vm, runAll);
  wrenReleaseHandle(vm, runErrorPropagate);
  wrenReleaseHandle(vm, nested);
  callNestedCleanup(vm);
  return exitCode;
}
