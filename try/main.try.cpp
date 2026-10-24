#include "../test/test.h"

#include <stdio.h>
#include <string.h>

//This is a simple program that exposes wren to the browser
//for https://wren.io/try and runs scripts.

// A try VM: prints to stdout, reports errors to stderr, and (under WREN_TRY,
// which this target is built with) serves no module sources.
struct TryVM : TestVM
{
  TryVM() : TestVM(false)
  {
  }
};

//The endpoint we call from the browser
int wren_compile(const char* input) {
  TryVM vm;
  WrenInterpretResult result = wrenInterpret(&vm, "compile", input);
  return (int)result;
}

//Main not used, but required. We call wren_compile directly.
int main(int argc, const char* argv[]) {
  return 0;
}
