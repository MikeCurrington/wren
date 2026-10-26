#ifndef wren_vm_h
#define wren_vm_h

#include "wren_common.h"
#include "wren_compiler.h"
#include "wren_value.h"
#include "wren_utils.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "../include/wren_debugger.h"

namespace wren
{
  class Module;
  class Handle;
}

// The maximum number of temporary objects that can be made visible to the GC
// at one time.
#define WREN_MAX_TEMP_ROOTS 8

typedef enum
{
  #define OPCODE(name, _) CODE_##name,
  #include "wren_opcodes.h"
  #undef OPCODE
} Code;

class WrenVM;

// A handle to a value, basically just a linked list of extra GC roots.
//
// Note that even non-heap-allocated values can be stored here.
class WrenHandle
{
public:
  WrenHandle(const WrenHandle&) = delete;
  WrenHandle& operator=(const WrenHandle&) = delete;
  WrenHandle(WrenHandle&&) = delete;
  WrenHandle& operator=(WrenHandle&&) = delete;

  // Creates a handle for [value] and links it into the VM's list of handles,
  // making the value reachable by the garbage collector. Defined in
  // wren_vm.cpp; the memory must come from ALLOCATE(), since like all Wren
  // objects a handle is never destructed -- wrenReleaseHandle() unlinks it
  // and frees it through the VM's allocator.
  WrenHandle(WrenVM* vm, Value value);

  // Internal state: read directly by the VM's implementation (handle list
  // management and the garbage collector).
  Value value {};

  WrenHandle* prev = nullptr;
  WrenHandle* next = nullptr;
};

// A saved region of API slots, used to re-enter the VM from within a foreign
// method. Created by wrenBeginCall() and destroyed by wrenEndCall().
//
// The base of the saved slot region is stored as an index into the fiber's
// stack rather than a pointer because the stack can be reallocated (moving)
// while the nested code runs.
class WrenCallContext
{
public:
  WrenCallContext(const WrenCallContext&) = delete;
  WrenCallContext& operator=(const WrenCallContext&) = delete;
  WrenCallContext(WrenCallContext&&) = delete;
  WrenCallContext& operator=(WrenCallContext&&) = delete;

  // Saves the current fiber and its slot region and pushes this context onto
  // the VM's stack of nested calls. Defined in wren_vm.cpp; the memory must
  // come from ALLOCATE(), since the context is never destructed --
  // wrenEndCall() pops it and frees it through the VM's allocator.
  WrenCallContext(WrenVM* vm);

  // Internal state: read directly by the VM's implementation (nested call
  // management and the garbage collector).

  // The fiber whose slot region was current when wrenBeginCall() was called.
  // This is the fiber that is suspended in the foreign method. It is kept
  // alive (and its stack scanned) by the GC while the nested code runs.
  ObjFiber* fiber;

  // The index of the base of the saved slot region in fiber->stack.
  int apiStackBase;

  // The next outer context, or NULL if this is the outermost one. The contexts
  // form a stack that parallels the C call stack.
  WrenCallContext* prev;
};

class WrenVM
{
public:
  WrenVM(const WrenVM&) = delete;
  WrenVM& operator=(const WrenVM&) = delete;
  WrenVM(WrenVM&&) = delete;
  WrenVM& operator=(WrenVM&&) = delete;

  // C++ host configuration: the plain Wren settings plus debugger options.
  struct Config
  {
    WrenConfiguration wrenConfig;

    // The port the debugger listens on (127.0.0.1) (if enabled)
    int debugPort = 4711;

    Config() { wrenInitConfiguration(&wrenConfig); }
  };

  explicit WrenVM(Config config = Config());
  virtual ~WrenVM();

  // ---------------------------------------------------------------------
  // Host callbacks. Subclasses override these to customize the VM; they
  // replace the function-pointer callbacks the old C API used to install
  // through WrenConfiguration.
  // ---------------------------------------------------------------------

  // Displays a string of text to the user (System.print and friends). The
  // default discards the text, mirroring it to the attached debugger's
  // console (if any).
  virtual void write(const char* text)
  {
#ifdef WREN_ENABLE_DEBUGGER
    if (debugger_) debugger_->writeOutput(text);
#else
    (void)text;
#endif
  }

  // Reports an error to the user. A compile-time error is reported once with
  // WREN_ERROR_COMPILE, the module and line where the error occurs, and the
  // compiler's message. A runtime error is reported with WREN_ERROR_RUNTIME
  // and no module or line, followed by one WREN_ERROR_STACK_TRACE call per
  // stack frame. The default discards errors, mirroring them to the attached
  // debugger's console (if any).
  virtual void error(WrenErrorType type, const char* module, int line,
                     const char* message)
  {
#ifdef WREN_ENABLE_DEBUGGER
    if (!debugger_) return;
    std::string text;
    if (type == WREN_ERROR_COMPILE)
    {
      text = "[" + std::string(module ? module : "?") + " line " +
             std::to_string(line) + "] " + message + "\n";
    }
    else if (type == WREN_ERROR_RUNTIME)
    {
      text = std::string(message) + "\n";
    }
    else
    {
      text = "[" + std::string(module ? module : "?") + " line " +
             std::to_string(line) + "] in " + message + "\n";
    }
    debugger_->writeOutput(text);
#else
    (void)type; (void)module; (void)line; (void)message;
#endif
  }

  // Gives the host a chance to canonicalize the imported module name,
  // potentially taking into account the name of the module that contains the
  // import. The default implements relative imports: a name starting with
  // "./" or "../" is resolved against the importing module's directory.
  // Return nullopt to report the import as a runtime error.
  virtual std::optional<std::string> resolveModule(std::string_view importer,
                                                   std::string_view name);

  // Loads and returns the source code for the module [name], or an empty
  // string if the module is not found. This is only called the first time a
  // given module name is imported. The default serves the source generated
  // for modules registered through module().
  virtual std::string loadModule(std::string_view name);

  // Returns a pointer to the foreign method for [signature] on [className]
  // in [module], or NULL if not found. The default looks up methods
  // registered through module()/Foreign<T>.
  virtual WrenForeignMethodFn bindForeignMethod(const char* module,
                                                const char* className,
                                                bool isStatic,
                                                const char* signature);

  // Returns the functions used to allocate and finalize instances of
  // [className] in [module]. The default looks up classes registered through
  // module()/Foreign<T>.
  virtual WrenForeignClassMethods bindForeignClass(const char* module,
                                                   const char* className);

  // Invoked from the interpreter loop on each new source line when a debug
  // hook is in use. The default forwards to the hook installed with
  // wrenSetDebugHook(), which is how the debugger attaches.
  virtual void onDebugEvent(WrenDebugEvent event)
  {
    if (debugHook != nullptr) debugHook(this, event, debugHookData);
  }

  // ---------------------------------------------------------------------
  // Host API.
  // ---------------------------------------------------------------------

  // The raw WrenVM (this object; kept for source compatibility with the
  // former wren::VM wrapper).
  WrenVM* raw() const { return const_cast<WrenVM*>(this); }

  // The VM's debugger, or nullptr.
  wren::debug::Debugger* debugger() const { return debugger_.get(); }

  void setDebugger(std::unique_ptr<wren::debug::Debugger> debugger)
  {
    debugger_ = std::move(debugger);
  }

  // Tells the debugger that Wren module [module] was loaded from [path], so
  // VS Code can map source files to module names for breakpoints. No-op
  // unless a debugger is attached. wren.hpp itself has no filesystem notion,
  // so file-loading hosts call this per module.
  void registerModulePath(std::string_view module, std::string_view path)
  {
    if (debugger_)
    {
      debugger_->registerModulePath(std::string(module), std::string(path));
    }
  }

  // Accesses (creating if needed) the module named [name], whose foreign
  // classes can be registered through Foreign<T>. Defined in wren.hpp.
  wren::Module& module(std::string name);

  WrenInterpretResult interpret(std::string_view mod, std::string_view src)
  {
    return interpretSource(std::string(mod).c_str(), std::string(src).c_str());
  }

  // A RAII handle for calling a method with [sig]. Defined in wren.hpp.
  wren::Handle callHandle(std::string_view sig);

  void getVariable(std::string_view mod, std::string_view name, int slot)
  {
    ensureSlots(slot + 1);
    lookupVariable(std::string(mod).c_str(), std::string(name).c_str(), slot);
  }

  void collectGarbage();
  void* userData() const { return config.userData; }
  void setUserData(void* data) { config.userData = data; }

  // Get a foreign object from a slot as a typed reference. Defined in
  // wren.hpp (needs the converters).
  template <typename T> T get(int slot);
  // Get a shared_ptr to a foreign object — enables C++ to keep the object
  // alive.
  template <typename T> std::shared_ptr<T> getShared(int slot);
  // Set a slot to a foreign object (copies/moves the value into a new
  // wrapper).
  template <typename T> void set(int slot, T&& value);
  // Set a slot from an existing shared_ptr — shares ownership, no copy.
  template <typename T> void setShared(int slot, std::shared_ptr<T> ptr);

  void setNull(int slot);
  int slotCount() const;
  void ensureSlots(int numSlots);
  WrenType slotType(int slot) const;

private:
  // ---------------------------------------------------------------------
  // Bodies of the exported C API. Each wren*() function declared in wren.h
  // is a one-line forwarder to a member here or to a public method above,
  // which holds the real implementation.
  // ---------------------------------------------------------------------

  // Slots (wren.h): reading and writing slot values.
  bool getSlotBool(int slot);
  const char* getSlotBytes(int slot, int* length);
  double getSlotDouble(int slot);
  void* getSlotForeign(int slot);
  const char* getSlotString(int slot);
  WrenHandle* getSlotHandle(int slot);
  void setSlotBool(int slot, bool value);
  void setSlotBytes(int slot, const char* bytes, size_t length);
  void setSlotDouble(int slot, double value);
  void* setSlotNewForeign(int slot, int classSlot, size_t size);
  void setSlotNewList(int slot);
  void setSlotNewMap(int slot);
  void setSlotString(int slot, const char* text);
  void setSlotHandle(int slot, WrenHandle* handle);

  // Slots (wren.h): list and map elements.
  int getListCount(int slot);
  void getListElement(int listSlot, int index, int elementSlot);
  void setListElement(int listSlot, int index, int elementSlot);
  void insertInList(int listSlot, int index, int elementSlot);
  int getMapCount(int slot);
  bool getMapContainsKey(int mapSlot, int keySlot);
  void getMapValue(int mapSlot, int keySlot, int valueSlot);
  void setMapValue(int mapSlot, int keySlot, int valueSlot);
  void removeMapValue(int mapSlot, int keySlot, int removedValueSlot);

  // Execution, handles, and module variables (wren.h).
  WrenInterpretResult interpretSource(const char* module, const char* source);
  WrenHandle* makeCallHandle(const char* signature);
  WrenInterpretResult call(WrenHandle* method);
  void releaseHandle(WrenHandle* handle);
  WrenCallContext* beginCall();
  void endCall(WrenCallContext* context);
  void abortFiber(int slot);
  bool hasVariable(const char* module, const char* name);
  bool hasModule(const char* module);
  void lookupVariable(const char* module, const char* name, int slot);

  // The exported C API functions in wren.h, which forward to the private
  // members above.
  friend bool wrenGetSlotBool(WrenVM* vm, int slot);
  friend const char* wrenGetSlotBytes(WrenVM* vm, int slot, int* length);
  friend double wrenGetSlotDouble(WrenVM* vm, int slot);
  friend void* wrenGetSlotForeign(WrenVM* vm, int slot);
  friend const char* wrenGetSlotString(WrenVM* vm, int slot);
  friend WrenHandle* wrenGetSlotHandle(WrenVM* vm, int slot);
  friend void wrenSetSlotBool(WrenVM* vm, int slot, bool value);
  friend void wrenSetSlotBytes(WrenVM* vm, int slot, const char* bytes,
                               size_t length);
  friend void wrenSetSlotDouble(WrenVM* vm, int slot, double value);
  friend void* wrenSetSlotNewForeign(WrenVM* vm, int slot, int classSlot,
                                     size_t size);
  friend void wrenSetSlotNewList(WrenVM* vm, int slot);
  friend void wrenSetSlotNewMap(WrenVM* vm, int slot);
  friend void wrenSetSlotString(WrenVM* vm, int slot, const char* text);
  friend void wrenSetSlotHandle(WrenVM* vm, int slot, WrenHandle* handle);
  friend int wrenGetListCount(WrenVM* vm, int slot);
  friend void wrenGetListElement(WrenVM* vm, int listSlot, int index,
                                 int elementSlot);
  friend void wrenSetListElement(WrenVM* vm, int listSlot, int index,
                                 int elementSlot);
  friend void wrenInsertInList(WrenVM* vm, int listSlot, int index,
                               int elementSlot);
  friend int wrenGetMapCount(WrenVM* vm, int slot);
  friend bool wrenGetMapContainsKey(WrenVM* vm, int mapSlot, int keySlot);
  friend void wrenGetMapValue(WrenVM* vm, int mapSlot, int keySlot,
                              int valueSlot);
  friend void wrenSetMapValue(WrenVM* vm, int mapSlot, int keySlot,
                              int valueSlot);
  friend void wrenRemoveMapValue(WrenVM* vm, int mapSlot, int keySlot,
                                 int removedValueSlot);
  friend WrenInterpretResult wrenInterpret(WrenVM* vm, const char* module,
                                           const char* source);
  friend WrenHandle* wrenMakeCallHandle(WrenVM* vm, const char* signature);
  friend WrenInterpretResult wrenCall(WrenVM* vm, WrenHandle* method);
  friend void wrenReleaseHandle(WrenVM* vm, WrenHandle* handle);
  friend WrenCallContext* wrenBeginCall(WrenVM* vm);
  friend void wrenEndCall(WrenVM* vm, WrenCallContext* context);
  friend void wrenAbortFiber(WrenVM* vm, int slot);
  friend bool wrenHasVariable(WrenVM* vm, const char* module,
                              const char* name);
  friend bool wrenHasModule(WrenVM* vm, const char* module);
  friend void wrenGetVariable(WrenVM* vm, const char* module,
                              const char* name, int slot);

  // ---------------------------------------------------------------------
  // State absorbed from the former wren::VM C++ wrapper. Only WrenVM's own
  // methods touch these.
  // ---------------------------------------------------------------------

  // The port the debugger listens on (127.0.0.1), from Config.
  int debugPort = 4711;

  // Foreign classes and methods registered from C++ through module(). The
  // default bindForeignMethod()/bindForeignClass()/loadModule()
  // implementations serve these. Module is defined in wren.hpp; it is only
  // complete where those are defined (wren.hpp / wren_vm.cpp).
  std::unordered_map<std::string, std::unique_ptr<wren::Module>> modules_;

  // The attached debugger, if any. Detached in ~WrenVM while the VM is still
  // valid, since it holds WrenHandles and the debug hook.
  std::unique_ptr<wren::debug::Debugger> debugger_;

public:
  // ---------------------------------------------------------------------
  // Internal interpreter state.
  //
  // Wren's own implementation (the interpreter loop, garbage collector,
  // compiler, primitives, and debug internals) reads and writes these fields
  // directly, so they remain public to it. They are not part of the host
  // API: hosts should work through the methods above.
  // ---------------------------------------------------------------------

  ObjClass* boolClass = nullptr;
  ObjClass* classClass = nullptr;
  ObjClass* fiberClass = nullptr;
  ObjClass* fnClass = nullptr;
  ObjClass* listClass = nullptr;
  ObjClass* mapClass = nullptr;
  ObjClass* nullClass = nullptr;
  ObjClass* numClass = nullptr;
  ObjClass* objectClass = nullptr;
  ObjClass* rangeClass = nullptr;
  ObjClass* stringClass = nullptr;

  // The fiber that is currently running.
  ObjFiber* fiber = nullptr;

  // The loaded modules. Each key is an ObjString (except for the main module,
  // whose key is null) for the module's name and the value is the ObjModule
  // for the module.
  ObjMap* modules = nullptr;
  
  // The most recently imported module. More specifically, the module whose
  // code has most recently finished executing.
  //
  // Not treated like a GC root since the module is already in [modules].
  ObjModule* lastModule = nullptr;

  // Memory management data:

  // The number of bytes that are known to be currently allocated. Includes all
  // memory that was proven live after the last GC, as well as any new bytes
  // that were allocated since then. Does *not* include bytes for objects that
  // were freed since the last GC.
  size_t bytesAllocated;

  // The number of total allocated bytes that will trigger the next GC.
  size_t nextGC = 0;

  // The first object in the linked list of all currently allocated objects.
  Obj* first = nullptr;

  // The "gray" set for the garbage collector. This is the stack of unprocessed
  // objects while a garbage collection pass is in process.
  std::vector<Obj*> gray;

  // The list of temporary roots. This is for temporary or new objects that are
  // not otherwise reachable but should not be collected.
  //
  // They are organized as a stack of pointers stored in this array. This
  // implies that temporary roots need to have stack semantics: only the most
  // recently pushed object can be released.
  Obj* tempRoots[WREN_MAX_TEMP_ROOTS] {};

  int numTempRoots= 0;
  
  // Pointer to the first node in the linked list of active handles or NULL if
  // there are none.
  WrenHandle* handles = nullptr;
  
  // Pointer to the bottom of the range of stack slots available for use from
  // the C API. During a foreign method, this will be in the stack of the fiber
  // that is executing a method.
  //
  // If not in a foreign method, this is initially NULL. If the user requests
  // slots by calling wrenEnsureSlots(), a stack is created and this is
  // initialized.
  Value* apiStack = nullptr;

  // The stack of nested API call contexts created by wrenBeginCall(). The head
  // is the innermost context. Each context's fiber is a fiber that is suspended
  // waiting for a foreign method to return, so they are all GC roots and may
  // not be resumed from Wren while they are in this list.
  //
  // This is NULL when there are no nested calls, which is the common case.
  WrenCallContext* callContexts = nullptr;

  WrenConfiguration config{};
  
  // Compiler and debugger data:

  // The compiler that is currently compiling code. This is used so that heap
  // allocated objects used by the compiler can be found if a GC is kicked off
  // in the middle of a compile.
  Compiler* compiler = nullptr;

  // There is a single global symbol table for all method names on all classes.
  // Method calls are dispatched directly by index in this table.
  SymbolTable methodNames {};

  // The debug hook, or NULL if debugging is not in use. When set, it is
  // invoked from the interpreter loop on each new source line. See the debug
  // API declarations in wren.h.
  WrenDebugHookFn debugHook {};

  // Opaque value passed back to the debug hook.
  void* debugHookData = nullptr;

  // While the debug hook is running, its API slots extend the fiber's stack
  // top. This remembers the real top so that the frame inspection functions
  // still report the frame's own slots. NULL when the hook isn't running.
  Value* debugSavedStackTop = nullptr;
};

// A generic allocation function that handles all explicit memory management.
// It's used like so:
//
// - To allocate new memory, [memory] is NULL and [oldSize] is zero. It should
//   return the allocated memory or NULL on failure.
//
// - To attempt to grow an existing allocation, [memory] is the memory,
//   [oldSize] is its previous size, and [newSize] is the desired size.
//   It should return [memory] if it was able to grow it in place, or a new
//   pointer if it had to move it.
//
// - To shrink memory, [memory], [oldSize], and [newSize] are the same as above
//   but it will always return [memory].
//
// - To free memory, [memory] will be the memory to free and [newSize] and
//   [oldSize] will be zero. It should return NULL.
void* wrenReallocate(WrenVM* vm, void* memory, size_t oldSize, size_t newSize);

// Invoke the finalizer for the foreign object referenced by [foreign].
void wrenFinalizeForeign(WrenVM* vm, ObjForeign* foreign);

// Creates a new [WrenHandle] for [value].
WrenHandle* wrenMakeHandle(WrenVM* vm, Value value);

// Compile [source] in the context of [module] and wrap in a fiber that can
// execute it.
//
// Returns NULL if a compile error occurred.
ObjClosure* wrenCompileSource(WrenVM* vm, const char* module,
                              const char* source, bool isExpression,
                              bool printErrors);

// Looks up a variable from a previously-loaded module.
//
// Aborts the current fiber if the module or variable could not be found.
Value wrenGetModuleVariable(WrenVM* vm, Value moduleName, Value variableName);

// Returns the value of the module-level variable named [name] in the main
// module.
Value wrenFindVariable(WrenVM* vm, ObjModule* module, const char* name);

// Adds a new implicitly declared top-level variable named [name] to [module]
// based on a use site occurring on [line].
//
// Does not check to see if a variable with that name is already declared or
// defined. Returns the symbol for the new variable or -2 if there are too many
// variables defined.
int wrenDeclareVariable(WrenVM* vm, ObjModule* module, const char* name,
                        size_t length, int line);

// Adds a new top-level variable named [name] to [module], and optionally
// populates line with the line of the implicit first use (line can be NULL).
//
// Returns the symbol for the new variable, -1 if a variable with the given name
// is already defined, or -2 if there are too many variables defined.
// Returns -3 if this is a top-level lowercase variable (localname) that was
// used before being defined.
int wrenDefineVariable(WrenVM* vm, ObjModule* module, const char* name,
                       size_t length, Value value, int* line);

// Pushes [closure] onto [fiber]'s callstack to invoke it. Expects [numArgs]
// arguments (including the receiver) to be on the top of the stack already.
static inline void wrenCallFunction(WrenVM* vm, ObjFiber* fiber,
                                    ObjClosure* closure, int numArgs)
{
  // Grow the call frame array if needed.
  if (fiber->numFrames + 1 > fiber->frameCapacity)
  {
    int max = fiber->frameCapacity * 2;
    fiber->frames = (CallFrame*)wrenReallocate(vm, fiber->frames,
        sizeof(CallFrame) * fiber->frameCapacity, sizeof(CallFrame) * max);
    fiber->frameCapacity = max;
  }
  
  // Grow the stack if needed.
  int stackSize = (int)(fiber->stackTop - fiber->stack);
  int needed = stackSize + closure->fn->maxSlots;
  wrenEnsureStack(vm, fiber, needed);
  
  wrenAppendCallFrame(vm, fiber, closure, fiber->stackTop - numArgs);
}

// Closes any open upvalues that have been created for stack slots at [last]
// and above in [fiber].
void wrenCloseUpvalues(ObjFiber* fiber, Value* last);

// Marks [obj] as a GC root so that it doesn't get collected.
void wrenPushRoot(WrenVM* vm, Obj* obj);

// Removes the most recently pushed temporary root.
void wrenPopRoot(WrenVM* vm);

// Returns the class of [value].
//
// Defined here instead of in wren_value.h because it's critical that this be
// inlined. That means it must be defined in the header, but the wren_value.h
// header doesn't have a full definitely of WrenVM yet.
static inline ObjClass* wrenGetClassInline(WrenVM* vm, Value value)
{
  if (IS_NUM(value)) return vm->numClass;
  if (IS_OBJ(value)) return AS_OBJ(value)->classObj;

#if WREN_NAN_TAGGING
  switch (GET_TAG(value))
  {
    case TAG_FALSE:     return vm->boolClass; break;
    case TAG_NAN:       return vm->numClass; break;
    case TAG_NULL:      return vm->nullClass; break;
    case TAG_TRUE:      return vm->boolClass; break;
    case TAG_UNDEFINED: UNREACHABLE();
  }
#else
  switch (value.type)
  {
    case VAL_FALSE:     return vm->boolClass;
    case VAL_NULL:      return vm->nullClass;
    case VAL_NUM:       return vm->numClass;
    case VAL_TRUE:      return vm->boolClass;
    case VAL_OBJ:       return AS_OBJ(value)->classObj;
    case VAL_UNDEFINED: UNREACHABLE();
  }
#endif

  UNREACHABLE();
  return nullptr;
}

// Returns `true` if [name] is a local variable name (starts with a lowercase
// letter).
static inline bool wrenIsLocalName(const char* name)
{
  return name[0] >= 'a' && name[0] <= 'z';
}

static inline bool wrenIsFalsyValue(Value value)
{
  return IS_FALSE(value) || IS_NULL(value);
}

#endif
