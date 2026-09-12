#ifndef wren_debug_h
#define wren_debug_h

#include "wren_value.h"
#include "wren_vm.h"

// Prints the stack trace for the current fiber.
//
// Used when a fiber throws a runtime error which is not caught.
void wrenDebugPrintStackTrace(WrenVM* vm);

// Records the names of the fields declared by a class's body, for the
// debugger. Called by the compiler at the end of each class definition;
// the names are copied. A later definition of the same class name replaces
// the recorded names, mirroring module variable redefinition.
void wrenDebugRecordClassFields(WrenVM* vm, const char* className,
                                const SymbolTable* names);

// Returns the name of field [index] of an instance of [classObj] at the
// given point, resolving inherited names through the superclass chain, or
// NULL if the name is unknown (e.g. a class the VM did not compile).
const char* wrenDebugGetFieldName(WrenVM* vm, ObjClass* classObj, int index);

// Frees the class field name registry for [vm]. Called from wrenFreeVM().
void wrenDebugClearClassFields(WrenVM* vm);

// Invokes the VM's debug hook (if installed) for the source line [frame] is
// about to execute. Called from the interpreter loop with the cached frame
// state already stored back into [frame]. Fires only on line transitions, not
// for every instruction. See also the debug API declarations in wren.h.
void wrenVmDebugHook(WrenVM* vm, ObjFiber* fiber, CallFrame* frame);

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
