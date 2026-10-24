#include <string.h>

#include "user_data.h"
#include "wren_vm.h"

static const char* data = "my user data";
static const char* otherData = "other user data";

void* testReallocateFn(void* ptr, size_t newSize, void* userData) {
  if (strcmp((const char*)userData, data) != 0) return NULL;

  if (newSize == 0)
  {
    free(ptr);
    return NULL;
  }

  return realloc(ptr, newSize);
}

static void test(WrenVM* vm)
{
  WrenVM::Config config;

  // Should default to NULL.
  if (config.wrenConfig.userData != NULL)
  {
    wrenSetSlotBool(vm, 0, false);
    return;
  }

  config.wrenConfig.reallocateFn = testReallocateFn;
  config.wrenConfig.userData = (void*)data;

  WrenVM otherVM(config);

  // Should be able to get it.
  if (wrenGetUserData(&otherVM) != data)
  {
    wrenSetSlotBool(vm, 0, false);
    return;
  }

  // Should be able to set it.
  wrenSetUserData(&otherVM, (void*)otherData);

  if (wrenGetUserData(&otherVM) != otherData)
  {
    wrenSetSlotBool(vm, 0, false);
    return;
  }

  wrenSetSlotBool(vm, 0, true);
}

WrenForeignMethodFn userDataBindMethod(const char* signature)
{
  if (strcmp(signature, "static UserData.test") == 0) return test;

  return NULL;
}
