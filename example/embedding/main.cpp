//For more details, visit https://wren.io/embedding/

#include <stdio.h>
#include "wren_vm.h"

// A WrenVM subclass that prints to stdout and reports errors to stdout.
// Host callbacks are virtual methods on the VM now: override the ones you
// want and construct the VM directly.
struct MyVM : WrenVM
{
  void write(const char* text) override
  {
    printf("%s", text);
  }

  void error(WrenErrorType errorType,
             const char* module, const int line,
             const char* msg) override
  {
    switch (errorType)
    {
      case WREN_ERROR_COMPILE:
      {
        printf("[%s line %d] [Error] %s\n", module, line, msg);
      } break;
      case WREN_ERROR_STACK_TRACE:
      {
        printf("[%s line %d] in %s\n", module, line, msg);
      } break;
      case WREN_ERROR_RUNTIME:
      {
        printf("[Runtime Error] %s\n", msg);
      } break;
    }
  }
};

int main()
{

  MyVM vm;

  const char* module = "main";
  const char* script = "System.print(\"I am running in a VM!\")";

  WrenInterpretResult result = wrenInterpret(&vm, module, script);

  switch (result)
  {
    case WREN_RESULT_COMPILE_ERROR:
      { printf("Compile Error!\n"); } break;
    case WREN_RESULT_RUNTIME_ERROR:
      { printf("Runtime Error!\n"); } break;
    case WREN_RESULT_SUCCESS:
      { printf("Success!\n"); } break;
  }

}
