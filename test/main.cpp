#include "./test.h"
#include "./api/api_tests.h"

#include <stdio.h>
#include <string.h>

static WrenVM* vm = NULL;

//This is a simple test runner that serves one purpose:
//To run the language level tests and benchmarks for Wren.
//It is not a general purpose vm or REPL.
//See wren-cli if you're looking for that.

static WrenVM* initVM(bool isAPITest)
{
  return new TestVM(isAPITest);
}

int main(int argc, const char* argv[]) {

  int handled = handle_args(argc, argv);
  if(handled != 0) return handled;

  int exitCode = 0;
  const char* testName = argv[1];
  bool isAPITest = isModuleAnAPITest(testName);

  vm = initVM(isAPITest);

  if(isAPITest) {
    APITest_Setup(vm, testName);
  }

  WrenInterpretResult result = runFile(vm, testName);

  if(isAPITest) {
    exitCode = APITest_Run(vm, testName);
  }

  delete vm;

  if (result == WREN_RESULT_COMPILE_ERROR) return WREN_EX_DATAERR;
  if (result == WREN_RESULT_RUNTIME_ERROR) return WREN_EX_SOFTWARE;

  return exitCode;

}

