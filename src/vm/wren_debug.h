#ifndef wren_debug_h
#define wren_debug_h

#include "wren_value.h"
#include "wren_vm.h"

// Prints the stack trace for the current fiber when a runtime error is not
// caught, records class field names for the debugger, resolves field names,
// and invokes the VM debug hook. These are all members of WrenVM; declared
// in wren_vm.h and defined in wren_debug.cpp.

// The "dump" functions are used for debugging Wren itself. Normal code paths
// will not call them unless one of the various DEBUG_ flags is enabled.

// Prints a representation of [value] to stdout.
void wrenDumpValue(Value value);

// Prints a representation of the bytecode for [fn] at instruction [i].
int wrenDumpInstruction(WrenVM* vm, ObjFn* fn, int i);

// Prints the disassembled code for [fn] to stdout.
void wrenDumpCode(WrenVM* vm, ObjFn* fn);

// Prints the contents of the current stack for [fiber] to stdout.
void wrenDumpStack(ObjFiber* fiber);

#endif
