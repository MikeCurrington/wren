#include <stdio.h>

#include "wren_debug.h"

void wrenDebugPrintStackTrace(WrenVM* vm)
{
  // Bail if the host doesn't enable printing errors.
  if (vm->config.errorFn == nullptr) return;
  
  ObjFiber* fiber = vm->fiber;
  if (IS_STRING(fiber->error))
  {
    vm->config.errorFn(vm, WREN_ERROR_RUNTIME,
                       nullptr, -1, AS_CSTRING(fiber->error));
  }
  else
  {
    // TODO: Print something a little useful here. Maybe the name of the error's
    // class?
    vm->config.errorFn(vm, WREN_ERROR_RUNTIME,
                       nullptr, -1, "[error object]");
  }

  for (int i = fiber->numFrames - 1; i >= 0; i--)
  {
    CallFrame* frame = &fiber->frames[i];
    ObjFn* fn = frame->closure->fn;

    // Skip over stub functions for calling methods from the C API.
    if (fn->module == nullptr) continue;
    
    // The built-in core module has no name. We explicitly omit it from stack
    // traces since we don't want to highlight to a user the implementation
    // detail of what part of the core module is written in C and what is Wren.
    if (fn->module->name == nullptr) continue;
    
    // -1 because IP has advanced past the instruction that it just executed.
    int line = fn->debug->sourceLines.data[frame->ip - fn->code.data - 1];
    vm->config.errorFn(vm, WREN_ERROR_STACK_TRACE,
                       fn->module->name->value, line,
                       fn->debug->name);
  }
}

static void dumpObject(Obj* obj)
{
  switch (obj->type)
  {
    case OBJ_CLASS:
      printf("[class %s %p]", ((ObjClass*)obj)->name->value, obj);
      break;
    case OBJ_CLOSURE: printf("[closure %p]", obj); break;
    case OBJ_FIBER: printf("[fiber %p]", obj); break;
    case OBJ_FN: printf("[fn %p]", obj); break;
    case OBJ_FOREIGN: printf("[foreign %p]", obj); break;
    case OBJ_INSTANCE: printf("[instance %p]", obj); break;
    case OBJ_LIST: printf("[list %p]", obj); break;
    case OBJ_MAP: printf("[map %p]", obj); break;
    case OBJ_MODULE: printf("[module %p]", obj); break;
    case OBJ_RANGE: printf("[range %p]", obj); break;
    case OBJ_STRING: printf("%s", ((ObjString*)obj)->value); break;
    case OBJ_UPVALUE: printf("[upvalue %p]", obj); break;
    default: printf("[unknown object %d]", obj->type); break;
  }
}

void wrenDumpValue(Value value)
{
#if WREN_NAN_TAGGING
  if (IS_NUM(value))
  {
    printf("%.14g", AS_NUM(value));
  }
  else if (IS_OBJ(value))
  {
    dumpObject(AS_OBJ(value));
  }
  else
  {
    switch (GET_TAG(value))
    {
      case TAG_FALSE:     printf("false"); break;
      case TAG_NAN:       printf("NaN"); break;
      case TAG_NULL:      printf("null"); break;
      case TAG_TRUE:      printf("true"); break;
      case TAG_UNDEFINED: UNREACHABLE();
    }
  }
#else
  switch (value.type)
  {
    case VAL_FALSE:     printf("false"); break;
    case VAL_NULL:      printf("null"); break;
    case VAL_NUM:       printf("%.14g", AS_NUM(value)); break;
    case VAL_TRUE:      printf("true"); break;
    case VAL_OBJ:       dumpObject(AS_OBJ(value)); break;
    case VAL_UNDEFINED: UNREACHABLE();
  }
#endif
}

static int dumpInstruction(WrenVM* vm, ObjFn* fn, int i, int* lastLine)
{
  int start = i;
  uint8_t* bytecode = fn->code.data;
  Code code = (Code)bytecode[i];

  int line = fn->debug->sourceLines.data[i];
  if (lastLine == nullptr || *lastLine != line)
  {
    printf("%4d:", line);
    if (lastLine != nullptr) *lastLine = line;
  }
  else
  {
    printf("     ");
  }

  printf(" %04d  ", i++);

  #define READ_BYTE() (bytecode[i++])
  #define READ_SHORT() (i += 2, (bytecode[i - 2] << 8) | bytecode[i - 1])

  #define BYTE_INSTRUCTION(name)                                               \
      printf("%-16s %5d\n", name, READ_BYTE());                                \
      break

  switch (code)
  {
    case CODE_CONSTANT:
    {
      int constant = READ_SHORT();
      printf("%-16s %5d '", "CONSTANT", constant);
      wrenDumpValue(fn->constants.data[constant]);
      printf("'\n");
      break;
    }

    case CODE_NULL:  printf("NULL\n"); break;
    case CODE_FALSE: printf("FALSE\n"); break;
    case CODE_TRUE:  printf("TRUE\n"); break;

    case CODE_LOAD_LOCAL_0: printf("LOAD_LOCAL_0\n"); break;
    case CODE_LOAD_LOCAL_1: printf("LOAD_LOCAL_1\n"); break;
    case CODE_LOAD_LOCAL_2: printf("LOAD_LOCAL_2\n"); break;
    case CODE_LOAD_LOCAL_3: printf("LOAD_LOCAL_3\n"); break;
    case CODE_LOAD_LOCAL_4: printf("LOAD_LOCAL_4\n"); break;
    case CODE_LOAD_LOCAL_5: printf("LOAD_LOCAL_5\n"); break;
    case CODE_LOAD_LOCAL_6: printf("LOAD_LOCAL_6\n"); break;
    case CODE_LOAD_LOCAL_7: printf("LOAD_LOCAL_7\n"); break;
    case CODE_LOAD_LOCAL_8: printf("LOAD_LOCAL_8\n"); break;

    case CODE_LOAD_LOCAL: BYTE_INSTRUCTION("LOAD_LOCAL");
    case CODE_STORE_LOCAL: BYTE_INSTRUCTION("STORE_LOCAL");
    case CODE_LOAD_UPVALUE: BYTE_INSTRUCTION("LOAD_UPVALUE");
    case CODE_STORE_UPVALUE: BYTE_INSTRUCTION("STORE_UPVALUE");

    case CODE_LOAD_MODULE_VAR:
    {
      int slot = READ_SHORT();
      printf("%-16s %5d '%s'\n", "LOAD_MODULE_VAR", slot,
             fn->module->variableNames.data[slot]->value);
      break;
    }

    case CODE_STORE_MODULE_VAR:
    {
      int slot = READ_SHORT();
      printf("%-16s %5d '%s'\n", "STORE_MODULE_VAR", slot,
             fn->module->variableNames.data[slot]->value);
      break;
    }

    case CODE_LOAD_FIELD_THIS: BYTE_INSTRUCTION("LOAD_FIELD_THIS");
    case CODE_STORE_FIELD_THIS: BYTE_INSTRUCTION("STORE_FIELD_THIS");
    case CODE_LOAD_FIELD: BYTE_INSTRUCTION("LOAD_FIELD");
    case CODE_STORE_FIELD: BYTE_INSTRUCTION("STORE_FIELD");

    case CODE_POP: printf("POP\n"); break;

    case CODE_CALL_0:
    case CODE_CALL_1:
    case CODE_CALL_2:
    case CODE_CALL_3:
    case CODE_CALL_4:
    case CODE_CALL_5:
    case CODE_CALL_6:
    case CODE_CALL_7:
    case CODE_CALL_8:
    case CODE_CALL_9:
    case CODE_CALL_10:
    case CODE_CALL_11:
    case CODE_CALL_12:
    case CODE_CALL_13:
    case CODE_CALL_14:
    case CODE_CALL_15:
    case CODE_CALL_16:
    {
      int numArgs = bytecode[i - 1] - CODE_CALL_0;
      int symbol = READ_SHORT();
      printf("CALL_%-11d %5d '%s'\n", numArgs, symbol,
             vm->methodNames.data[symbol]->value);
      break;
    }

    case CODE_SUPER_0:
    case CODE_SUPER_1:
    case CODE_SUPER_2:
    case CODE_SUPER_3:
    case CODE_SUPER_4:
    case CODE_SUPER_5:
    case CODE_SUPER_6:
    case CODE_SUPER_7:
    case CODE_SUPER_8:
    case CODE_SUPER_9:
    case CODE_SUPER_10:
    case CODE_SUPER_11:
    case CODE_SUPER_12:
    case CODE_SUPER_13:
    case CODE_SUPER_14:
    case CODE_SUPER_15:
    case CODE_SUPER_16:
    {
      int numArgs = bytecode[i - 1] - CODE_SUPER_0;
      int symbol = READ_SHORT();
      int superclass = READ_SHORT();
      printf("SUPER_%-10d %5d '%s' %5d\n", numArgs, symbol,
             vm->methodNames.data[symbol]->value, superclass);
      break;
    }

    case CODE_JUMP:
    {
      int offset = READ_SHORT();
      printf("%-16s %5d to %d\n", "JUMP", offset, i + offset);
      break;
    }

    case CODE_LOOP:
    {
      int offset = READ_SHORT();
      printf("%-16s %5d to %d\n", "LOOP", offset, i - offset);
      break;
    }

    case CODE_JUMP_IF:
    {
      int offset = READ_SHORT();
      printf("%-16s %5d to %d\n", "JUMP_IF", offset, i + offset);
      break;
    }

    case CODE_AND:
    {
      int offset = READ_SHORT();
      printf("%-16s %5d to %d\n", "AND", offset, i + offset);
      break;
    }

    case CODE_OR:
    {
      int offset = READ_SHORT();
      printf("%-16s %5d to %d\n", "OR", offset, i + offset);
      break;
    }

    case CODE_CLOSE_UPVALUE: printf("CLOSE_UPVALUE\n"); break;
    case CODE_RETURN:        printf("RETURN\n"); break;

    case CODE_CLOSURE:
    {
      int constant = READ_SHORT();
      printf("%-16s %5d ", "CLOSURE", constant);
      wrenDumpValue(fn->constants.data[constant]);
      printf(" ");
      ObjFn* loadedFn = AS_FN(fn->constants.data[constant]);
      for (int j = 0; j < loadedFn->numUpvalues; j++)
      {
        int isLocal = READ_BYTE();
        int index = READ_BYTE();
        if (j > 0) printf(", ");
        printf("%s %d", isLocal ? "local" : "upvalue", index);
      }
      printf("\n");
      break;
    }

    case CODE_CONSTRUCT:         printf("CONSTRUCT\n"); break;
    case CODE_FOREIGN_CONSTRUCT: printf("FOREIGN_CONSTRUCT\n"); break;
      
    case CODE_CLASS:
    {
      int numFields = READ_BYTE();
      printf("%-16s %5d fields\n", "CLASS", numFields);
      break;
    }

    case CODE_FOREIGN_CLASS: printf("FOREIGN_CLASS\n"); break;
    case CODE_END_CLASS: printf("END_CLASS\n"); break;

    case CODE_METHOD_INSTANCE:
    {
      int symbol = READ_SHORT();
      printf("%-16s %5d '%s'\n", "METHOD_INSTANCE", symbol,
             vm->methodNames.data[symbol]->value);
      break;
    }

    case CODE_METHOD_STATIC:
    {
      int symbol = READ_SHORT();
      printf("%-16s %5d '%s'\n", "METHOD_STATIC", symbol,
             vm->methodNames.data[symbol]->value);
      break;
    }
      
    case CODE_END_MODULE:
      printf("END_MODULE\n");
      break;
      
    case CODE_IMPORT_MODULE:
    {
      int name = READ_SHORT();
      printf("%-16s %5d '", "IMPORT_MODULE", name);
      wrenDumpValue(fn->constants.data[name]);
      printf("'\n");
      break;
    }
      
    case CODE_IMPORT_VARIABLE:
    {
      int variable = READ_SHORT();
      printf("%-16s %5d '", "IMPORT_VARIABLE", variable);
      wrenDumpValue(fn->constants.data[variable]);
      printf("'\n");
      break;
    }
      
    case CODE_END:
      printf("END\n");
      break;

    default:
      printf("UKNOWN! [%d]\n", bytecode[i - 1]);
      break;
  }

  // Return how many bytes this instruction takes, or -1 if it's an END.
  if (code == CODE_END) return -1;
  return i - start;

  #undef READ_BYTE
  #undef READ_SHORT
}

int wrenDumpInstruction(WrenVM* vm, ObjFn* fn, int i)
{
  return dumpInstruction(vm, fn, i, nullptr);
}

void wrenDumpCode(WrenVM* vm, ObjFn* fn)
{
  printf("%s: %s\n",
         fn->module->name == nullptr ? "<core>" : fn->module->name->value,
         fn->debug->name);

  int i = 0;
  int lastLine = -1;
  for (;;)
  {
    int offset = dumpInstruction(vm, fn, i, &lastLine);
    if (offset == -1) break;
    i += offset;
  }

  printf("\n");
}

void wrenDumpStack(ObjFiber* fiber)
{
  printf("(fiber %p) ", fiber);
  for (Value* slot = fiber->stack; slot < fiber->stackTop; slot++)
  {
    wrenDumpValue(*slot);
    printf(" | ");
  }
  printf("\n");
}

// ---------------------------------------------------------------------------
// Debug hook support
// ---------------------------------------------------------------------------

// Called from the interpreter loop's DISPATCH() macro when a debug hook is
// installed, with the interpreter's cached frame state already stored back
// into [frame].
//
// Fires the hook once per line transition (including backward jumps to an
// earlier instruction on the same line, so one-line loop bodies still report)
// rather than on every instruction.
//
// While the user hook runs, the API slots are wired to the top of the fiber's
// stack so that the hook can inspect the paused state through the standard
// slot API, exactly like a foreign method can. They are unhooked on return.
void wrenVmDebugHook(WrenVM* vm, ObjFiber* fiber, CallFrame* frame)
{
  ObjFn* fn = frame->closure->fn;
  if (fn->debug == nullptr) return;

  int offset = (int)(frame->ip - fn->code.data);
  if (offset < 0 || offset >= fn->code.count) return;

  int line = fn->debug->sourceLines.data[offset];

  if (line == frame->debugLastLine && offset >= frame->debugLastOffset) return;
  frame->debugLastLine = line;
  frame->debugLastOffset = offset;

  int apiStackBase = (int)(fiber->stackTop - fiber->stack);
  vm->apiStack = fiber->stackTop;
  vm->debugSavedStackTop = fiber->stackTop;

  vm->debugHook(vm, WREN_DEBUG_LINE, vm->debugHookData);

  vm->debugSavedStackTop = nullptr;
  vm->apiStack = nullptr;
  fiber->stackTop = fiber->stack + apiStackBase;
}

void wrenSetDebugHook(WrenVM* vm, WrenDebugHookFn fn, void* userData)
{
  vm->debugHook = fn;
  vm->debugHookData = userData;
}

// Returns the call frame numbered the way the public debug API exposes frames:
// innermost frame is 0. Returns NULL if [index] is out of range.
static CallFrame* debugGetFrame(WrenVM* vm, int index)
{
  if (vm->fiber == nullptr) return nullptr;
  if (index < 0 || index >= vm->fiber->numFrames) return nullptr;
  return &vm->fiber->frames[vm->fiber->numFrames - 1 - index];
}

int wrenDebugGetFrameCount(WrenVM* vm)
{
  if (vm->fiber == nullptr) return 0;
  return vm->fiber->numFrames;
}

bool wrenDebugGetFrameInfo(WrenVM* vm, int frame, WrenDebugFrameInfo* info)
{
  CallFrame* callFrame = debugGetFrame(vm, frame);
  if (callFrame == nullptr || info == nullptr) return false;

  ObjFn* fn = callFrame->closure->fn;

  info->module = "?";
  info->function = "?";
  info->line = 0;

  // Stub functions for calling methods from the C API have no module, and the
  // core module's functions have no module name.
  if (fn->module != nullptr && fn->module->name != nullptr)
  {
    info->module = fn->module->name->value;
  }

  if (fn->debug != nullptr)
  {
    if (fn->debug->name != nullptr) info->function = fn->debug->name;

    int offset = (int)(callFrame->ip - fn->code.data);
    if (offset >= 0 && offset < fn->debug->sourceLines.count)
    {
      info->line = fn->debug->sourceLines.data[offset];
    }
  }

  return true;
}

// Returns a pointer one past the last stack slot used by the frame at
// [frameIndex] (which indexes from the bottom of the call stack, unlike the
// public API).
static Value* debugFrameLocalEnd(WrenVM* vm, ObjFiber* fiber, int frameIndex)
{
  // The innermost frame's slots end at the stack top -- except while the
  // debug hook's own API slots are wired up, which temporarily extend it.
  if (frameIndex == fiber->numFrames - 1)
  {
    if (vm->debugSavedStackTop != nullptr) return vm->debugSavedStackTop;
    return fiber->stackTop;
  }

  return fiber->frames[frameIndex + 1].stackStart;
}

int wrenDebugGetLocalCount(WrenVM* vm, int frame)
{
  CallFrame* callFrame = debugGetFrame(vm, frame);
  if (callFrame == nullptr) return 0;

  Value* end = debugFrameLocalEnd(vm, vm->fiber,
                                  vm->fiber->numFrames - 1 - frame);
  return (int)(end - callFrame->stackStart);
}

void wrenDebugGetLocal(WrenVM* vm, int frame, int index, int slot)
{
  CallFrame* callFrame = debugGetFrame(vm, frame);
  ASSERT(callFrame != nullptr, "Frame index out of range.");

  Value* end = debugFrameLocalEnd(vm, vm->fiber,
                                  vm->fiber->numFrames - 1 - frame);
  ASSERT(index >= 0 && callFrame->stackStart + index < end,
         "Local index out of range.");
  ASSERT(slot >= 0 && slot < wrenGetSlotCount(vm), "Not that many slots.");

  vm->apiStack[slot] = callFrame->stackStart[index];
}

// Returns the module of the function executing in call frame [frame], numbered
// the way the public debug API exposes frames.
static ObjModule* debugGetFrameModule(WrenVM* vm, int frame)
{
  CallFrame* callFrame = debugGetFrame(vm, frame);
  if (callFrame == nullptr) return nullptr;
  return callFrame->closure->fn->module;
}

int wrenDebugGetModuleVariableCount(WrenVM* vm, int frame)
{
  ObjModule* module = debugGetFrameModule(vm, frame);
  if (module == nullptr) return 0;
  return module->variableNames.count;
}

const char* wrenDebugGetModuleVariableName(WrenVM* vm, int frame, int index)
{
  ObjModule* module = debugGetFrameModule(vm, frame);
  ASSERT(module != nullptr, "Frame index out of range.");
  ASSERT(index >= 0 && index < module->variableNames.count,
         "Variable index out of range.");
  return module->variableNames.data[index]->value;
}

void wrenDebugGetModuleVariable(WrenVM* vm, int frame, int index, int slot)
{
  ObjModule* module = debugGetFrameModule(vm, frame);
  ASSERT(module != nullptr, "Frame index out of range.");
  ASSERT(index >= 0 && index < module->variables.count,
         "Variable index out of range.");
  ASSERT(slot >= 0 && slot < wrenGetSlotCount(vm), "Not that many slots.");

  vm->apiStack[slot] = module->variables.data[index];
}
