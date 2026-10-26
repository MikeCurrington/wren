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
  virtual void Write(const char* text)
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
  virtual void Error(WrenErrorType type, const char* module, int line,
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
  virtual std::optional<std::string> ResolveModule(std::string_view importer,
                                                   std::string_view name);

  // Loads and returns the source code for the module [name], or an empty
  // string if the module is not found. This is only called the first time a
  // given module name is imported. The default serves the source generated
  // for modules registered through module().
  virtual std::string LoadModule(std::string_view name);

  // Returns a pointer to the foreign method for [signature] on [className]
  // in [module], or NULL if not found. The default looks up methods
  // registered through module()/Foreign<T>.
  virtual WrenForeignMethodFn BindForeignMethod(const char* module,
                                                const char* className,
                                                bool isStatic,
                                                const char* signature);

  // Returns the functions used to allocate and finalize instances of
  // [className] in [module]. The default looks up classes registered through
  // module()/Foreign<T>.
  virtual WrenForeignClassMethods BindForeignClass(const char* module,
                                                   const char* className);

  // Invoked from the interpreter loop on each new source line when a debug
  // hook is in use. The default forwards to the hook installed with
  // wrenSetDebugHook(), which is how the debugger attaches.
  virtual void OnDebugEvent(WrenDebugEvent event)
  {
    if (debugHook != nullptr) debugHook(this, event, debugHookData);
  }

  // ---------------------------------------------------------------------
  // Host API.
  // ---------------------------------------------------------------------

  // The raw WrenVM (this object; kept for source compatibility with the
  // former wren::VM wrapper).
  WrenVM* Raw() const { return const_cast<WrenVM*>(this); }

  // The VM's debugger, or nullptr.
  wren::debug::Debugger* Debugger() const { return debugger_.get(); }

  void SetDebugger(std::unique_ptr<wren::debug::Debugger> debugger)
  {
    debugger_ = std::move(debugger);
  }

  // Tells the debugger that Wren module [module] was loaded from [path], so
  // VS Code can map source files to module names for breakpoints. No-op
  // unless a debugger is attached. wren.hpp itself has no filesystem notion,
  // so file-loading hosts call this per module.
  void RegisterModulePath(std::string_view module, std::string_view path)
  {
    if (debugger_)
    {
      debugger_->registerModulePath(std::string(module), std::string(path));
    }
  }

  // Accesses (creating if needed) the module named [name], whose foreign
  // classes can be registered through Foreign<T>. Defined in wren.hpp.
  wren::Module& Module(std::string name);

  WrenInterpretResult Interpret(std::string_view mod, std::string_view src)
  {
    return InterpretSource(std::string(mod).c_str(), std::string(src).c_str());
  }

  // A RAII handle for calling a method with [sig]. Defined in wren.hpp.
  wren::Handle CallHandle(std::string_view sig);

  void GetVariable(std::string_view mod, std::string_view name, int slot)
  {
    EnsureSlots(slot + 1);
    LookupVariable(std::string(mod).c_str(), std::string(name).c_str(), slot);
  }

  void CollectGarbage();
  void* UserData() const;
  void SetUserData(void* data);

  // Get a foreign object from a slot as a typed reference. Defined in
  // wren.hpp (needs the converters).
  template <typename T> T Get(int slot);
  // Get a shared_ptr to a foreign object — enables C++ to keep the object
  // alive.
  template <typename T> std::shared_ptr<T> GetShared(int slot);
  // Set a slot to a foreign object (copies/moves the value into a new
  // wrapper).
  template <typename T> void Set(int slot, T&& value);
  // Set a slot from an existing shared_ptr — shares ownership, no copy.
  template <typename T> void SetShared(int slot, std::shared_ptr<T> ptr);

  void SetNull(int slot);
  int SlotCount() const;
  void EnsureSlots(int numSlots);
  WrenType SlotType(int slot) const;

  // ---------------------------------------------------------------------
  // VM implementation. The rest of Wren's implementation (core library,
  // compiler, garbage collector, and debugger) calls these; they are not
  // part of the host API.
  // ---------------------------------------------------------------------

  // Generic allocation through the VM; see the wrenReallocate() notes below.
  void* Reallocate(void* memory, size_t oldSize, size_t newSize);

  // GC temporary roots.
  void PushRoot(Obj* obj);
  void PopRoot();

  // Creates a new [WrenHandle] for [value].
  WrenHandle* MakeHandle(Value value);

  // Compiles [source] in the context of [module]. Returns NULL if a compile
  // error occurred.
  ObjClosure* CompileSource(const char* module, const char* source,
                            bool isExpression, bool printErrors);

  // Looks up a variable from a previously-loaded module. Aborts the current
  // fiber if the module or variable could not be found.
  Value GetModuleVariable(Value moduleName, Value variableName);

  // Returns the value of the module-level variable named [name] in [module].
  Value FindVariable(ObjModule* module, const char* name);

  // Adds a new implicitly declared top-level variable named [name] to
  // [module] from a use site on [line]. Returns the symbol, or -2 if there
  // are too many variables defined.
  int DeclareVariable(ObjModule* module, const char* name, size_t length,
                      int line);

  // Adds a new top-level variable named [name] to [module]. Returns the
  // symbol, -1 if already defined, -2 if too many, or -3 if a localname was
  // used before being defined.
  int DefineVariable(ObjModule* module, const char* name, size_t length,
                     Value value, int* line);

  // Object factories and value operations (wren_value.cpp).
  ObjClass* NewSingleClass(int numFields, ObjString* name);
  void BindSuperclass(ObjClass* subclass, ObjClass* superclass);
  ObjClass* NewClass(ObjClass* superclass, int numFields, ObjString* name);
  void AddMethod(ObjClass* classObj, int symbol, Method method);
  void EnsureStack(ObjFiber* fiber, int needed);
  void FunctionBindName(ObjFn* fn, const char* name, int length);
  void ListInsert(ObjList* list, Value value, uint32_t index);
  Value ListRemoveAt(ObjList* list, uint32_t index);
  void MapSet(ObjMap* map, Value key, Value value);
  void MapClear(ObjMap* map);
  Value MapRemoveKey(ObjMap* map, Value key);
  Value NewString(const char* text);
  Value NewStringLength(const char* text, size_t length);
  Value NewStringFromRange(ObjString* source, int start, uint32_t count,
                           int step);
  Value NumToString(double value);
  Value StringFormat(const char* format, ...);
  Value StringFromCodePoint(int value);
  Value StringFromByte(uint8_t value);
  Value StringCodePointAt(ObjString* string, uint32_t index);
  ObjClass* GetClass(Value value);

  // Garbage collector (wren_value.cpp).
  void GrayObj(Obj* obj);
  void GrayValue(Value value);
  void GrayBuffer(ValueBuffer* buffer);
  void BlackenObjects();
  void FreeObj(Obj* obj);

  // Core library bootstrap (wren_core.cpp). Must run once in the constructor
  // before any code executes.
  void InitializeCore();

  // Resumes [fiber], as invoked by the fiber primitives in wren_core.cpp.
  bool RunFiber(ObjFiber* fiber, Value* args, bool isCall, bool hasValue,
                const char* verb);

  // Invokes [closure] (which must be a plain function) with [numArgs]
  // arguments, from the fn.call primitives in wren_core.cpp.
  void CallFn(Value* args, int numArgs);

  // Compiles [source] in [module] and returns the function, or NULL on a
  // compile error (wren_compiler.cpp).
  ObjFn* Compile(ObjModule* module, const char* source, bool isExpression,
                 bool printErrors);

  // Marks any objects the compiler is using as GC roots (wren_compiler.cpp).
  void MarkCompiler(Compiler* compiler);

  // Debug support (wren_debug.cpp).
  void PrintStackTrace();
  void DumpCode(ObjFn* fn);
  void VmDebugHook(ObjFiber* fiber, CallFrame* frame);
  void RecordClassFields(const char* className, const SymbolTable* names);
  const char* GetFieldName(ObjClass* classObj, int index);
  void ClearClassFields();

  // Invokes the finalizer for [foreign].
  void FinalizeForeign(ObjForeign* foreign);

  // Pushes [closure] onto [fiber]'s callstack to invoke it. Expects [numArgs]
  // arguments (including the receiver) to be on the top of the stack already.
  void CallFunction(ObjFiber* fiber, ObjClosure* closure, int numArgs);

  // Returns the class of [value]. Must stay inline; defined after the class.
  ObjClass* GetClassInline(Value value);

  // The currently running fiber. Exposed for the primitives and foreign
  // method bodies, which cannot be class members because their signature is
  // fixed by the Primitive/WrenForeignMethodFn function-pointer typedefs.
  ObjFiber* Fiber() const { return fiber; }
  void SetFiber(ObjFiber* runningFiber) { fiber = runningFiber; }

  // The C API slot region (see the slot functions in wren.h). Same rationale
  // as Fiber(): the primitives, the optional modules, and the debugger's
  // debug hook inspect slots directly.
  Value* ApiStack() const { return apiStack; }
  void SetApiStack(Value* stack) { apiStack = stack; }

  // The map of loaded modules, keyed the way GetModule() looks them up.
  // Exposed for the meta module's foreign methods.
  ObjMap* Modules() const { return modules; }

  // Registers/unregisters the compiler whose objects the GC must find, while
  // a compilation is in progress. Used by the compiler's free helper
  // functions, which cannot be members.
  void SetCompiler(Compiler* compiler) { this->compiler = compiler; }

  // Adds [name] to the interned method name table, returning its index.
  // Exposed for the compiler's free helper functions.
  int EnsureMethodName(const char* name, size_t length)
  {
    return methodNames.ensure(this, name, length);
  }

  // Accounts [bytes] of still-live allocation during GC marking. Exposed for
  // SymbolTable::blacken(), whose buffer storage lives outside the VM.
  void ChargeBytes(size_t bytes) { bytesAllocated += bytes; }

private:
  // ---------------------------------------------------------------------
  // Bodies of the exported C API. Each wren*() function declared in wren.h
  // is a one-line forwarder to a member here or to a public method above,
  // which holds the real implementation.
  // ---------------------------------------------------------------------

  // Slots (wren.h): reading and writing slot values.
  bool GetSlotBool(int slot);
  const char* GetSlotBytes(int slot, int* length);
  double GetSlotDouble(int slot);
  void* GetSlotForeign(int slot);
  const char* GetSlotString(int slot);
  WrenHandle* GetSlotHandle(int slot);
  void SetSlotBool(int slot, bool value);
  void SetSlotBytes(int slot, const char* bytes, size_t length);
  void SetSlotDouble(int slot, double value);
  void* SetSlotNewForeign(int slot, int classSlot, size_t size);
  void SetSlotNewList(int slot);
  void SetSlotNewMap(int slot);
  void SetSlotString(int slot, const char* text);
  void SetSlotHandle(int slot, WrenHandle* handle);

  // Slots (wren.h): list and map elements.
  int GetListCount(int slot);
  void GetListElement(int listSlot, int index, int elementSlot);
  void SetListElement(int listSlot, int index, int elementSlot);
  void InsertInList(int listSlot, int index, int elementSlot);
  int GetMapCount(int slot);
  bool GetMapContainsKey(int mapSlot, int keySlot);
  void GetMapValue(int mapSlot, int keySlot, int valueSlot);
  void SetMapValue(int mapSlot, int keySlot, int valueSlot);
  void RemoveMapValue(int mapSlot, int keySlot, int removedValueSlot);

  // Execution, handles, and module variables (wren.h).
  WrenInterpretResult InterpretSource(const char* module, const char* source);
  WrenHandle* MakeCallHandle(const char* signature);
  WrenInterpretResult Call(WrenHandle* method);
  void ReleaseHandle(WrenHandle* handle);
  WrenCallContext* BeginCall();
  void EndCall(WrenCallContext* context);
  void AbortFiber(int slot);
  bool HasVariable(const char* module, const char* name);
  bool HasModule(const char* module);
  void LookupVariable(const char* module, const char* name, int slot);

  // Debug API (wren.h; defined in wren_debug.cpp).
  void SetDebugHook(WrenDebugHookFn fn, void* userData);
  int DebugGetFrameCount();
  bool DebugGetFrameInfo(int frame, WrenDebugFrameInfo* info);
  int DebugGetLocalCount(int frame);
  void DebugGetLocal(int frame, int index, int slot);
  const char* DebugGetLocalName(int frame, int index);
  int DebugGetModuleVariableCount(int frame);
  const char* DebugGetModuleVariableName(int frame, int index);
  void DebugGetModuleVariable(int frame, int index, int slot);

  // Interpreter and module machinery (wren_vm.cpp).
  WrenInterpretResult RunInterpreter(ObjFiber* fiber);
  void RuntimeError();
  void MethodNotFound(ObjClass* classObj, int symbol);
  void CallForeign(ObjFiber* fiber, WrenForeignMethodFn foreign,
                   int numArgs);
  bool CheckArity(Value value, int numArgs);
  ObjUpvalue* CaptureUpvalue(ObjFiber* fiber, Value* local);
  WrenForeignMethodFn FindForeignMethod(const char* moduleName,
      const char* className, bool isStatic, const char* signature);
  void BindMethod(int methodType, int symbol, ObjModule* module,
                  ObjClass* classObj, Value methodValue);
  ObjModule* GetModule(Value name);
  ObjClosure* CompileInModule(Value name, const char* source,
                              bool isExpression, bool printErrors);
  Value ValidateSuperclass(Value name, Value superclassValue, int numFields);
  void NewForeign(ObjClass* classObj, ObjModule* module);
  void EndClass();
  void CreateClass(int numFields, ObjModule* module);
  void CreateForeign(ObjFiber* fiber, Value* stack);
  Value ResolveModuleName(Value name);
  Value ImportModule(Value name);
  Value GetModuleVariable(ObjModule* module, Value variableName);
  void ValidateApiSlot(int slot) const;
  void SetSlot(int slot, Value value);

  // wren_value.cpp internals.
  ObjString* AllocateString(size_t length);
  void ResizeMap(ObjMap* map, uint32_t capacity);
  void BlackenClass(ObjClass* classObj);
  void BlackenClosure(ObjClosure* closure);
  void BlackenFiber(ObjFiber* fiber);
  void BlackenFn(ObjFn* fn);
  void BlackenForeign(ObjForeign* foreign);
  void BlackenInstance(ObjInstance* instance);
  void BlackenList(ObjList* list);
  void BlackenMap(ObjMap* map);
  void BlackenModule(ObjModule* module);
  void BlackenRange(ObjRange* range);
  void BlackenString(ObjString* string);
  void BlackenUpvalue(ObjUpvalue* upvalue);
  void BlackenObject(Obj* obj);

  // wren_core.cpp internals.
  ObjClass* DefineClass(ObjModule* module, const char* name);

  // wren_debug.cpp internals.
  CallFrame* DebugGetFrame(int index, ObjFiber** owner);
  Value* DebugFrameLocalEnd(ObjFiber* fiber, CallFrame* callFrame);
  ObjModule* DebugFrameModule(int frame);
  int DumpInstruction(ObjFn* fn, int i);
  int DumpInstruction(ObjFn* fn, int i, int* lastLine);

  // The core library primitives. They are static members so that they can
  // access private VM state while still being stored in method tables
  // through the Primitive function-pointer typedef. Declared from
  // wren_core_prims.inc, defined and registered in wren_core.cpp.
#define PRIM(name) static bool Prim##name(WrenVM* vm, Value* args);
#include "wren_core_prims.inc"
#undef PRIM

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
  friend void wrenSetDebugHook(WrenVM* vm, WrenDebugHookFn fn,
                               void* userData);
  friend int wrenDebugGetFrameCount(WrenVM* vm);
  friend bool wrenDebugGetFrameInfo(WrenVM* vm, int frame,
                                    WrenDebugFrameInfo* info);
  friend int wrenDebugGetLocalCount(WrenVM* vm, int frame);
  friend void wrenDebugGetLocal(WrenVM* vm, int frame, int index, int slot);
  friend const char* wrenDebugGetLocalName(WrenVM* vm, int frame, int index);
  friend int wrenDebugGetModuleVariableCount(WrenVM* vm, int frame);
  friend const char* wrenDebugGetModuleVariableName(WrenVM* vm, int frame,
                                                    int index);
  friend void wrenDebugGetModuleVariable(WrenVM* vm, int frame, int index,
                                         int slot);

  // The Obj constructors link each new object into the VM's object list and
  // read the core class pointers.
  friend struct Obj;
  friend struct ObjString;
  friend struct ObjUpvalue;
  friend struct ObjModule;
  friend struct ObjFn;
  friend struct ObjClosure;
  friend struct ObjFiber;
  friend struct ObjClass;
  friend struct ObjForeign;
  friend struct ObjInstance;
  friend struct ObjList;
  friend struct ObjMap;
  friend struct ObjRange;

  // The handle and call context constructors link themselves into the VM's
  // lists.
  friend class WrenHandle;
  friend class WrenCallContext;

  // The compiler registers itself in [compiler] and adds to [methodNames].
  friend struct sCompiler;

  // State absorbed from the former wren::VM C++ wrapper. Only WrenVM's own
  // methods touch these.
  // ---------------------------------------------------------------------

  // The port the debugger listens on (127.0.0.1), from Config.
  int debugPort = 4711;

  // Foreign classes and methods registered from C++ through module(). The
  // default BindForeignMethod()/BindForeignClass()/LoadModule()
  // implementations serve these. Module is defined in wren.hpp; it is only
  // complete where those are defined (wren.hpp / wren_vm.cpp).
  std::unordered_map<std::string, std::unique_ptr<wren::Module>> modules_;

  // The attached debugger, if any. Detached in ~WrenVM while the VM is still
  // valid, since it holds WrenHandles and the debug hook.
  std::unique_ptr<wren::debug::Debugger> debugger_;

  // ---------------------------------------------------------------------
  // Internal interpreter state. Private: only WrenVM's own member functions
  // (and the friends declared above) read and write these fields. They are
  // not part of the host API: hosts work through the methods above.
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

// Pushes [closure] onto [fiber]'s callstack to invoke it. Expects [numArgs]
// arguments (including the receiver) to be on the top of the stack already.
inline void WrenVM::CallFunction(ObjFiber* fiber,
                                   ObjClosure* closure, int numArgs)
{
  // Grow the call frame array if needed.
  if (fiber->numFrames + 1 > fiber->frameCapacity)
  {
    int max = fiber->frameCapacity * 2;
    fiber->frames = (CallFrame*)Reallocate(fiber->frames,
        sizeof(CallFrame) * fiber->frameCapacity, sizeof(CallFrame) * max);
    fiber->frameCapacity = max;
  }
  
  // Grow the stack if needed.
  int stackSize = (int)(fiber->stackTop - fiber->stack);
  int needed = stackSize + closure->fn->maxSlots;
  EnsureStack(fiber, needed);
  
  wrenAppendCallFrame(fiber, closure, fiber->stackTop - numArgs);
}

// Closes any open upvalues that have been created for stack slots at [last]
// and above in [fiber].
void wrenCloseUpvalues(ObjFiber* fiber, Value* last);

// Returns the class of [value].
//
// Defined here instead of in wren_value.h because it's critical that this be
// inlined. That means it must be defined in the header, but the wren_value.h
// header doesn't have a full definitely of WrenVM yet.
inline ObjClass* WrenVM::GetClassInline(Value value)
{
  if (IS_NUM(value)) return numClass;
  if (IS_OBJ(value)) return AS_OBJ(value)->classObj;

#if WREN_NAN_TAGGING
  switch (GET_TAG(value))
  {
    case TAG_FALSE:     return boolClass; break;
    case TAG_NAN:       return numClass; break;
    case TAG_NULL:      return nullClass; break;
    case TAG_TRUE:      return boolClass; break;
    case TAG_UNDEFINED: UNREACHABLE();
  }
#else
  switch (value.type)
  {
    case VAL_FALSE:     return boolClass;
    case VAL_NULL:      return nullClass;
    case VAL_NUM:       return numClass;
    case VAL_TRUE:      return boolClass;
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
