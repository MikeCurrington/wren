#include <string.h>
#include <time.h>

#include "benchmark.h"
#include "wren_vm.h"

static void arguments(WrenVM* vm)
{
  double result = 0;

  result += wrenGetSlotDouble(vm, 1);
  result += wrenGetSlotDouble(vm, 2);
  result += wrenGetSlotDouble(vm, 3);
  result += wrenGetSlotDouble(vm, 4);

  wrenSetSlotDouble(vm, 0, result);
}

const char* testScript =
"class Test {\n"
"  static method(a, b, c, d) { a + b + c + d }\n"
"}\n";

static void call(WrenVM* vm)
{
  int iterations = (int)wrenGetSlotDouble(vm, 1);

  // Since the VM is not re-entrant, we can't call from within this foreign
  // method. Instead, make a new VM to run the call test in.
  WrenVM otherVM;

  wrenInterpret(&otherVM, "main", testScript);

  WrenHandle* method = wrenMakeCallHandle(&otherVM, "method(_,_,_,_)");

  wrenEnsureSlots(&otherVM, 1);
  wrenGetVariable(&otherVM, "main", "Test", 0);
  WrenHandle* testClass = wrenGetSlotHandle(&otherVM, 0);

  double startTime = (double)clock() / CLOCKS_PER_SEC;

  double result = 0;
  for (int i = 0; i < iterations; i++)
  {
    wrenEnsureSlots(&otherVM, 5);
    wrenSetSlotHandle(&otherVM, 0, testClass);
    wrenSetSlotDouble(&otherVM, 1, 1.0);
    wrenSetSlotDouble(&otherVM, 2, 2.0);
    wrenSetSlotDouble(&otherVM, 3, 3.0);
    wrenSetSlotDouble(&otherVM, 4, 4.0);

    wrenCall(&otherVM, method);

    result += wrenGetSlotDouble(&otherVM, 0);
  }

  double elapsed = (double)clock() / CLOCKS_PER_SEC - startTime;

  wrenReleaseHandle(&otherVM, testClass);
  wrenReleaseHandle(&otherVM, method);

  if (result == (1.0 + 2.0 + 3.0 + 4.0) * iterations)
  {
    wrenSetSlotDouble(vm, 0, elapsed);
  }
  else
  {
    // Got the wrong result.
    wrenSetSlotBool(vm, 0, false);
  }
}

static void accumulatorAllocate(WrenVM* vm)
{
  vm->SetSlotNewForeignShared(0, 0,
      std::shared_ptr<double>(new double(wrenGetSlotDouble(vm, 1)),
          std::default_delete<double>()), nullptr);
}

static void accumulatorInc(WrenVM* vm)
{
  double* value = static_cast<double*>(vm->GetSlotForeign(0));
  (*value)++;
  wrenSetSlotDouble(vm, 0, *value);
}

static void accumulatorValue(WrenVM* vm)
{
  double value = *static_cast<double*>(vm->GetSlotForeign(0));
  wrenSetSlotDouble(vm, 0, value);
}

static void slotsSum(WrenVM* vm)
{
  int count = wrenGetListCount(vm, 1);

  double result = 0;
  wrenEnsureSlots(vm, 3);
  for (int i = 0; i < count; i++)
  {
    wrenGetListElement(vm, 1, i, 2);
    result += wrenGetSlotDouble(vm, 2);
  }

  wrenSetSlotDouble(vm, 0, result);
}

static void slotsBuild(WrenVM* vm)
{
  int count = (int)wrenGetSlotDouble(vm, 1);

  wrenSetSlotNewList(vm, 0);
  wrenEnsureSlots(vm, 2);
  for (int i = 1; i <= count; i++)
  {
    wrenSetSlotDouble(vm, 1, i * 1.0);
    wrenInsertInList(vm, 0, -1, 1);
  }
}

WrenForeignMethodFn benchmarkBindMethod(const char* signature)
{
  if (strcmp(signature, "static Benchmark.arguments(_,_,_,_)") == 0) return arguments;
  if (strcmp(signature, "static Benchmark.call(_)") == 0) return call;
  if (strcmp(signature, "Accumulator.inc()") == 0) return accumulatorInc;
  if (strcmp(signature, "Accumulator.value") == 0) return accumulatorValue;
  if (strcmp(signature, "static Slots.sum(_)") == 0) return slotsSum;
  if (strcmp(signature, "static Slots.build(_)") == 0) return slotsBuild;

  return NULL;
}

void benchmarkBindClass(const char* className, WrenForeignClassMethods* methods)
{
  if (strcmp(className, "Accumulator") == 0)
  {
    methods->allocate = accumulatorAllocate;
    return;
  }
}
