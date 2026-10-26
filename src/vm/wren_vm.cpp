#include <stdarg.h>
#include <string.h>
#include <vector>

#include "wren.h"
#include "wren_common.h"
#include "wren_compiler.h"
#include "wren_core.h"
#include "wren_debug.h"
#include "wren_primitive.h"
#include "wren_vm.h"

// For the complete wren::Module (the modules_ registry is destroyed here and
// the default BindForeignMethod()/BindForeignClass()/LoadModule()
// implementations look up registrations in it).
#include "../include/wren.hpp"

#if WREN_OPT_META
  #include "wren_opt_meta.h"
#endif
#if WREN_OPT_RANDOM
  #include "wren_opt_random.h"
#endif

#if WREN_DEBUG_TRACE_MEMORY || WREN_DEBUG_TRACE_GC
  #include <time.h>
  #include <stdio.h>
#endif

// The behavior of realloc() when the size is 0 is implementation defined. It
// may return a non-NULL pointer which must not be dereferenced but nevertheless
// should be freed. To prevent that, we avoid calling realloc() with a zero
// size.
static void* defaultReallocate(void* ptr, size_t newSize, void* _)
{
  if (newSize == 0)
  {
    free(ptr);
    return nullptr;
  }

  return realloc(ptr, newSize);
}

int wrenGetVersionNumber() 
{ 
  return WREN_VERSION_NUMBER;
}

void wrenInitConfiguration(WrenConfiguration* config)
{
  config->reallocateFn = defaultReallocate;
  config->initialHeapSize = 1024 * 1024 * 10;
  config->minHeapSize = 1024 * 1024;
  config->heapGrowthPercent = 50;
  config->userData = nullptr;
}

WrenVM::WrenVM(Config config)
{
  // Copy the configuration, ensuring a real allocator.
  WrenReallocateFn reallocate = config.wrenConfig.reallocateFn != nullptr
      ? config.wrenConfig.reallocateFn : defaultReallocate;
  memcpy(&this->config, &config.wrenConfig, sizeof(WrenConfiguration));
  this->config.reallocateFn = reallocate;
  this->debugPort = config.debugPort;

  // TODO: Tune this.
  this->gray.reserve(4);
  this->nextGC = this->config.initialHeapSize;

  this->modules = wrenConstruct<ObjMap>(this, 0);
  wrenInitializeCore(this);
}

WrenVM::~WrenVM()
{
  // The debugger keeps WrenHandles alive and installs a debug hook, so it
  // must detach while the VM is still valid.
  debugger_.reset();

  ASSERT(methodNames.count() > 0, "VM appears to have already been freed.");

  // Free all of the GC objects.
  Obj* obj = first;
  while (obj != nullptr)
  {
    Obj* next = obj->next;
    FreeObj( obj);
    obj = next;
  }

  // Tell the user if they didn't free any handles. We don't want to just free
  // them here because the host app may still have pointers to them that they
  // may try to use. Better to tell them about the bug early.
  ASSERT(handles == nullptr, "All handles have not been released.");

  // Likewise, all nested calls must have been ended before the VM is freed.
  ASSERT(callContexts == nullptr, "All nested calls have not been ended.");

  methodNames.clear(this);

  wrenDebugClearClassFields(this);
}

std::optional<std::string> WrenVM::ResolveModule(std::string_view importer,
                                                 std::string_view name)
{
  // Logical import strings are used as-is and need no resolution.
  bool isPath = !name.empty() && (name.front() == '/' ||
      name.substr(0, 2) == "./" || name.substr(0, 3) == "../");
  if (!isPath) return std::string(name);

  // Get the directory containing the importing module.
  std::string path(importer);
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) path.clear();
  else path.resize(slash);

  // Add the relative import path.
  if (!path.empty() && path.back() != '/') path += '/';
  path += name;

  // Normalize: resolve "." and ".." components. A leading "/" is preserved,
  // ".." components that walk out of the tree are kept (except for absolute
  // paths), and a leading "./" is preserved so that path and logical imports
  // remain distinguishable.
  const bool isAbsolute = !path.empty() && path.front() == '/';
  const bool leadingDotSlash = path.compare(0, 2, "./") == 0;
  std::vector<std::string_view> components;
  int leadingDoubles = 0;
  size_t pos = 0;
  while (pos <= path.size())
  {
    size_t next = path.find('/', pos);
    if (next == std::string::npos) next = path.size();
    std::string_view component =
        std::string_view(path).substr(pos, next - pos);
    if (component == ".")
    {
      // Skip "." components.
    }
    else if (component == "..")
    {
      // Walk out of directories on "..".
      if (!components.empty()) components.pop_back();
      else if (!isAbsolute) leadingDoubles++;
    }
    else if (!component.empty())
    {
      components.push_back(component);
    }
    pos = next + 1;
  }

  std::string result;
  if (isAbsolute)
  {
    result = "/";
    for (const auto& component : components)
    {
      if (result.back() != '/') result += '/';
      result += component;
    }
  }
  else
  {
    if (leadingDoubles > 0)
    {
      for (int i = 0; i < leadingDoubles; i++)
      {
        if (!result.empty()) result += '/';
        result += "..";
      }
    }
    else if (leadingDotSlash)
    {
      result = ".";
    }
    for (const auto& component : components)
    {
      if (!result.empty()) result += '/';
      result += component;
    }
  }

  if (result.empty()) result = ".";
  return result;
}

std::string WrenVM::LoadModule(std::string_view name)
{
  // Serve the source generated for modules registered from C++.
  auto it = modules_.find(std::string(name));
  if (it == modules_.end()) return std::string();
  return it->second->generateSource();
}

WrenForeignMethodFn WrenVM::BindForeignMethod(const char* module,
                                              const char* className,
                                              bool isStatic,
                                              const char* signature)
{
  auto it = modules_.find(module);
  if (it == modules_.end()) return nullptr;
  for (const auto& c : it->second->registration().classes)
  {
    if (c.className != className) continue;
    // Exact match first.
    for (const auto& m : c.methods)
    {
      if (m.isStatic == isStatic && m.signature == signature) return m.fn;
    }
    // Fallback: Wren distinguishes getter "foo" from call "foo()" for 0-arg
    // methods. Allow either form to match a 0-arg method registration.
    std::string reqSig(signature);
    std::string altSig;
    if (reqSig.size() > 2 && reqSig.compare(reqSig.size() - 2, 2, "()") == 0)
    {
      altSig = reqSig.substr(0, reqSig.size() - 2); // "foo()" -> "foo"
    }
    else if (reqSig.find('(') == std::string::npos)
    {
      altSig = reqSig + "()"; // "foo" -> "foo()"
    }
    if (!altSig.empty())
    {
      for (const auto& m : c.methods)
      {
        if (m.isStatic == isStatic && m.signature == altSig) return m.fn;
      }
    }
  }
  return nullptr;
}

WrenForeignClassMethods WrenVM::BindForeignClass(const char* module,
                                                 const char* className)
{
  WrenForeignClassMethods methods{nullptr, nullptr};
  auto it = modules_.find(module);
  if (it == modules_.end()) return methods;
  for (const auto& c : it->second->registration().classes)
  {
    if (c.className != className) continue;
    methods.allocate = c.allocator;
    methods.finalize = c.finalizer;
    break;
  }
  return methods;
}

void WrenVM::CollectGarbage()
{
#if WREN_DEBUG_TRACE_MEMORY || WREN_DEBUG_TRACE_GC
  printf("-- gc --\n");

  size_t before = this->bytesAllocated;
  double startTime = (double)clock() / CLOCKS_PER_SEC;
#endif

  // Mark all reachable objects.

  // Reset this. As we mark objects, their size will be counted again so that
  // we can track how much memory is in use without needing to know the size
  // of each *freed* object.
  //
  // This is important because when freeing an unmarked object, we don't always
  // know how much memory it is using. For example, when freeing an instance,
  // we need to know its class to know how big it is, but its class may have
  // already been freed.
  this->bytesAllocated = 0;

  GrayObj( (Obj*)this->modules);

  // Temporary roots.
  for (int i = 0; i < this->numTempRoots; i++)
  {
    GrayObj( this->tempRoots[i]);
  }

  // The current fiber.
  GrayObj( (Obj*)this->fiber);

  // Fibers that are suspended in foreign methods waiting for nested calls to
  // complete. They are only reachable from the C stack, so the VM has to
  // track them explicitly or they (and the values on their stacks) would be
  // collected while the nested code runs.
  for (WrenCallContext* context = this->callContexts;
       context != nullptr;
       context = context->prev)
  {
    GrayObj( (Obj*)context->fiber);
  }

  // The handles.
  for (WrenHandle* handle = this->handles;
       handle != nullptr;
       handle = handle->next)
  {
    GrayValue( handle->value);
  }

  // Any object the compiler is using (if there is one).
  if (this->compiler != nullptr) wrenMarkCompiler(this, this->compiler);

  // Method names.
  this->methodNames.blacken(this);

  // Now that we have grayed the roots, do a depth-first search over all of the
  // reachable objects.
  BlackenObjects();

  // Collect the white objects.
  Obj** obj = &this->first;
  while (*obj != nullptr)
  {
    if (!((*obj)->isDark))
    {
      // This object wasn't reached, so remove it from the list and free it.
      Obj* unreached = *obj;
      *obj = unreached->next;
      FreeObj( unreached);
    }
    else
    {
      // This object was reached, so unmark it (for the next GC) and move on to
      // the next.
      (*obj)->isDark = false;
      obj = &(*obj)->next;
    }
  }

  // Calculate the next gc point, this is the current allocation plus
  // a configured percentage of the current allocation.
  this->nextGC = this->bytesAllocated + ((this->bytesAllocated * this->config.heapGrowthPercent) / 100);
  if (this->nextGC < this->config.minHeapSize) this->nextGC = this->config.minHeapSize;

#if WREN_DEBUG_TRACE_MEMORY || WREN_DEBUG_TRACE_GC
  double elapsed = ((double)clock() / CLOCKS_PER_SEC) - startTime;
  // Explicit cast because size_t has different sizes on 32-bit and 64-bit and
  // we need a consistent type for the format string.
  printf("GC %lu before, %lu after (%lu collected), next at %lu. Took %.3fms.\n",
         (unsigned long)before,
         (unsigned long)this->bytesAllocated,
         (unsigned long)(before - this->bytesAllocated),
         (unsigned long)this->nextGC,
         elapsed*1000.0);
#endif
}

void* WrenVM::Reallocate(void* memory, size_t oldSize, size_t newSize)
{
#if WREN_DEBUG_TRACE_MEMORY
  // Explicit cast because size_t has different sizes on 32-bit and 64-bit and
  // we need a consistent type for the format string.
  printf("reallocate %p %lu -> %lu\n",
         memory, (unsigned long)oldSize, (unsigned long)newSize);
#endif

  // If new bytes are being allocated, add them to the total count. If objects
  // are being completely deallocated, we don't track that (since we don't
  // track the original size). Instead, that will be handled while marking
  // during the next GC.
  this->bytesAllocated += newSize - oldSize;

#if WREN_DEBUG_GC_STRESS
  // Since collecting calls this function to free things, make sure we don't
  // recurse.
  if (newSize > 0) wrenCollectGarbage(this);
#else
  if (newSize > 0 && this->bytesAllocated > this->nextGC) wrenCollectGarbage(this);
#endif

  return this->config.reallocateFn(memory, newSize, this->config.userData);
}

// Captures the local variable [local] into an [Upvalue]. If that local is
// already in an upvalue, the existing one will be used. (This is important to
// ensure that multiple closures closing over the same variable actually see
// the same variable.) Otherwise, it will create a new open upvalue and add it
// the fiber's list of upvalues.
ObjUpvalue* WrenVM::CaptureUpvalue(ObjFiber* fiber, Value* local)
{
  // If there are no open upvalues at all, we must need a new one.
  if (fiber->openUpvalues == nullptr)
  {
    fiber->openUpvalues = wrenConstruct<ObjUpvalue>(this, 0, local);
    return fiber->openUpvalues;
  }

  ObjUpvalue* prevUpvalue = nullptr;
  ObjUpvalue* upvalue = fiber->openUpvalues;

  // Walk towards the bottom of the stack until we find a previously existing
  // upvalue or pass where it should be.
  while (upvalue != nullptr && upvalue->value > local)
  {
    prevUpvalue = upvalue;
    upvalue = upvalue->next;
  }

  // Found an existing upvalue for this local.
  if (upvalue != nullptr && upvalue->value == local) return upvalue;

  // We've walked past this local on the stack, so there must not be an
  // upvalue for it already. Make a new one and link it in in the right
  // place to keep the list sorted.
  ObjUpvalue* createdUpvalue = wrenConstruct<ObjUpvalue>(this, 0, local);
  if (prevUpvalue == nullptr)
  {
    // The new one is the first one in the list.
    fiber->openUpvalues = createdUpvalue;
  }
  else
  {
    prevUpvalue->next = createdUpvalue;
  }

  createdUpvalue->next = upvalue;
  return createdUpvalue;
}

// Closes any open upvalues that have been created for stack slots at [last]
// and above.
void wrenCloseUpvalues(ObjFiber* fiber, Value* last)
{
  while (fiber->openUpvalues != nullptr &&
         fiber->openUpvalues->value >= last)
  {
    ObjUpvalue* upvalue = fiber->openUpvalues;

    // Move the value into the upvalue itself and point the upvalue to it.
    upvalue->closed = *upvalue->value;
    upvalue->value = &upvalue->closed;

    // Remove it from the open upvalue list.
    fiber->openUpvalues = upvalue->next;
  }
}

// Looks up a foreign method in [moduleName] on [className] with [signature].
//
// This will try the host's foreign method binder first. If that fails, it
// falls back to handling the built-in modules.
WrenForeignMethodFn WrenVM::FindForeignMethod(
                                             const char* moduleName,
                                             const char* className,
                                             bool isStatic,
                                             const char* signature)
{
  WrenForeignMethodFn method = this->BindForeignMethod(moduleName, className,
                                                     isStatic, signature);

  // If the host didn't provide it, see if it's an optional one.
  if (method == nullptr)
  {
#if WREN_OPT_META
    if (strcmp(moduleName, "meta") == 0)
    {
      method = wrenMetaBindForeignMethod(this, className, isStatic, signature);
    }
#endif
#if WREN_OPT_RANDOM
    if (strcmp(moduleName, "random") == 0)
    {
      method = wrenRandomBindForeignMethod(this, className, isStatic, signature);
    }
#endif
  }

  return method;
}

// Defines [methodValue] as a method on [classObj].
//
// Handles both foreign methods where [methodValue] is a string containing the
// method's signature and Wren methods where [methodValue] is a function.
//
// Aborts the current fiber if the method is a foreign method that could not be
// found.
void WrenVM::BindMethod(int methodType, int symbol,
                       ObjModule* module, ObjClass* classObj, Value methodValue)
{
  const char* className = classObj->name->value;
  if (methodType == CODE_METHOD_STATIC) classObj = classObj->classObj;

  Method method;
  if (IS_STRING(methodValue))
  {
    const char* name = AS_CSTRING(methodValue);
    method.type = METHOD_FOREIGN;
    method.as.foreign = FindForeignMethod(module->name->value,
                                          className,
                                          methodType == CODE_METHOD_STATIC,
                                          name);

    if (method.as.foreign == nullptr)
    {
      this->fiber->error = StringFormat( 
          "Could not find foreign method '@' for class $ in module '$'.",
          methodValue, classObj->name->value, module->name->value);
      return;
    }
  }
  else
  {
    method.as.closure = AS_CLOSURE(methodValue);
    method.type = METHOD_BLOCK;

    // Patch up the bytecode now that we know the superclass.
    wrenBindMethodCode(classObj, method.as.closure->fn);
  }

  AddMethod( classObj, symbol, method);
}

void WrenVM::CallForeign(ObjFiber* fiber,
                        WrenForeignMethodFn foreign, int numArgs)
{
  // Store the base of the API slot region as an index into the fiber's stack
  // instead of a pointer. The foreign method may call back into Wren, and any
  // of that nested code can grow (and reallocate) some fiber's stack. Since a
  // fiber suspended in a foreign call can never be resumed, this fiber's stack
  // cannot move, so the index is all that's needed to find the region again.
  int apiStackBase = (int)(fiber->stackTop - fiber->stack) - numArgs;

  this->apiStack = fiber->stackTop - numArgs;

  foreign(this);

  // If the foreign method called back into Wren using wrenBeginCall() but
  // didn't end the nested call -- because it errored or simply forgot -- end
  // any remaining ones now. Their slot regions belong to this fiber and are no
  // longer meaningful once the foreign method has returned.
  while (this->callContexts != nullptr && this->callContexts->fiber == fiber)
  {
    wrenEndCall(this, this->callContexts);
  }

  // Restore the VM's state. A nested call may have changed the current fiber
  // or aborted the VM entirely.
  this->fiber = fiber;
  this->apiStack = nullptr;

  // Discard the stack slots for the arguments and temporaries but leave one
  // for the result.
  fiber->stackTop = fiber->stack + apiStackBase + 1;
}

// Handles the current fiber having aborted because of an error.
//
// Walks the call chain of fibers, aborting each one until it hits a fiber that
// handles the error. If none do, tells the VM to stop.
void WrenVM::RuntimeError()
{
  ASSERT(wrenHasError(this->fiber), "Should only call this after an error.");

  ObjFiber* current = this->fiber;
  Value error = current->error;
  
  while (current != nullptr)
  {
    // Every fiber along the call chain gets aborted with the same error.
    current->error = error;

    // If the caller ran this fiber using "try", give it the error and stop.
    if (current->state == FIBER_TRY)
    {
      // Make the caller's try method return the error message.
      current->caller->stackTop[-1] = this->fiber->error;
      this->fiber = current->caller;
      return;
    }
    
    // Otherwise, unhook the caller since we will never resume and return to it.
    ObjFiber* caller = current->caller;
    current->caller = nullptr;
    current = caller;
  }

  // If we got here, nothing caught the error, so show the stack trace.
  wrenDebugPrintStackTrace(this);
  this->fiber = nullptr;
  this->apiStack = nullptr;
}

// Aborts the current fiber with an appropriate method not found error for a
// method with [symbol] on [classObj].
void WrenVM::MethodNotFound(ObjClass* classObj, int symbol)
{
  this->fiber->error = StringFormat( "@ does not implement '$'.",
      OBJ_VAL(classObj->name), this->methodNames[symbol]->value);
}

// Looks up the previously loaded module with [name].
//
// Returns `NULL` if no module with that name has been loaded.
ObjModule* WrenVM::GetModule(Value name)
{
  Value moduleValue = wrenMapGet(this->modules, name);
  return !IS_UNDEFINED(moduleValue) ? AS_MODULE(moduleValue) : nullptr;
}

ObjClosure* WrenVM::CompileInModule(Value name, const char* source,
                                   bool isExpression, bool printErrors)
{
  // See if the module has already been loaded.
  ObjModule* module = GetModule(name);
  if (module == nullptr)
  {
    module = wrenConstruct<ObjModule>(this, 0, AS_STRING(name));

    // It's possible for the wrenMapSet below to resize the modules map,
    // and trigger a GC while doing so. When this happens it will collect
    // the module we've just created. Once in the map it is safe.
    PushRoot((Obj*)module);

    // Store it in the VM's module registry so we don't load the same module
    // multiple times.
    MapSet( this->modules, name, OBJ_VAL(module));

    PopRoot();

    // Implicitly import the core module.
    ObjModule* coreModule = GetModule(NULL_VAL);
    for (int i = 0; i < coreModule->variables.size(); i++)
    {
      DefineVariable(module,
                         coreModule->variableNames[i]->value,
                         coreModule->variableNames[i]->length,
                         coreModule->variables[i], nullptr);
    }
  }

  ObjFn* fn = wrenCompile(this, module, source, isExpression, printErrors);
  if (fn == nullptr)
  {
    // TODO: Should we still store the module even if it didn't compile?
    return nullptr;
  }

  // Functions are always wrapped in closures.
  PushRoot((Obj*)fn);
  ObjClosure* closure = wrenConstruct<ObjClosure>(
      this, sizeof(ObjUpvalue*) * fn->numUpvalues, fn);
  PopRoot(); // fn.

  return closure;
}

// Verifies that [superclassValue] is a valid object to inherit from. That
// means it must be a class and cannot be the class of any built-in type.
//
// Also validates that it doesn't result in a class with too many fields and
// the other limitations foreign classes have.
//
// If successful, returns `null`. Otherwise, returns a string for the runtime
// error message.
Value WrenVM::ValidateSuperclass(Value name, Value superclassValue,
                                int numFields)
{
  // Make sure the superclass is a class.
  if (!IS_CLASS(superclassValue))
  {
    return StringFormat( 
        "Class '@' cannot inherit from a non-class object.",
        name);
  }

  // Make sure it doesn't inherit from a sealed built-in type. Primitive methods
  // on these classes assume the instance is one of the other Obj___ types and
  // will fail horribly if it's actually an ObjInstance.
  ObjClass* superclass = AS_CLASS(superclassValue);
  if (superclass == this->classClass ||
      superclass == this->fiberClass ||
      superclass == this->fnClass || // Includes OBJ_CLOSURE.
      superclass == this->listClass ||
      superclass == this->mapClass ||
      superclass == this->rangeClass ||
      superclass == this->stringClass ||
      superclass == this->boolClass ||
      superclass == this->nullClass ||
      superclass == this->numClass)
  {
    return StringFormat( 
        "Class '@' cannot inherit from built-in class '@'.",
        name, OBJ_VAL(superclass->name));
  }

  if (superclass->numFields == -1)
  {
    return StringFormat( 
        "Class '@' cannot inherit from foreign class '@'.",
        name, OBJ_VAL(superclass->name));
  }

  if (numFields == -1 && superclass->numFields > 0)
  {
    return StringFormat( 
        "Foreign class '@' may not inherit from a class with fields.",
        name);
  }

  if (superclass->numFields + numFields > MAX_FIELDS)
  {
    return StringFormat( 
        "Class '@' may not have more than 255 fields, including inherited "
        "ones.", name);
  }

  return NULL_VAL;
}

void WrenVM::NewForeign(ObjClass* classObj, ObjModule* module)
{
  // Let the host bind the class first so it can override the built-in
  // optional modules.
  WrenForeignClassMethods methods = this->BindForeignClass(module->name->value,
                                                         classObj->name->value);

  // If the host didn't provide it, see if it's a built in optional module.
  if (methods.allocate == nullptr && methods.finalize == nullptr)
  {
#if WREN_OPT_RANDOM
    if (strcmp(module->name->value, "random") == 0)
    {
      methods = wrenRandomBindForeignClass(this, module->name->value,
                                           classObj->name->value);
    }
#endif
  }
  
  Method method;
  method.type = METHOD_FOREIGN;

  // Add the symbol even if there is no allocator so we can ensure that the
  // symbol itself is always in the symbol table.
  int symbol = this->methodNames.ensure(this, "<allocate>", 10);
  if (methods.allocate != nullptr)
  {
    method.as.foreign = methods.allocate;
    AddMethod( classObj, symbol, method);
  }
  
  // Add the symbol even if there is no finalizer so we can ensure that the
  // symbol itself is always in the symbol table.
  symbol = this->methodNames.ensure(this, "<finalize>", 10);
  if (methods.finalize != nullptr)
  {
    method.as.foreign = (WrenForeignMethodFn)methods.finalize;
    AddMethod( classObj, symbol, method);
  }
}

// Completes the process for creating a new class.
//
// The class attributes instance and the class itself should be on the 
// top of the fiber's stack. 
//
// This process handles moving the attribute data for a class from
// compile time to runtime, since it now has all the attributes associated
// with a class, including for methods.
void WrenVM::EndClass() 
{
  // Pull the attributes and class off the stack
  Value attributes = this->fiber->stackTop[-2];
  Value classValue = this->fiber->stackTop[-1];

  // Remove the stack items
  this->fiber->stackTop -= 2;

  ObjClass* classObj = AS_CLASS(classValue);
    classObj->attributes = attributes;
}

// Creates a new class.
//
// If [numFields] is -1, the class is a foreign class. The name and superclass
// should be on top of the fiber's stack. After calling this, the top of the
// stack will contain the new class.
//
// Aborts the current fiber if an error occurs.
void WrenVM::CreateClass(int numFields, ObjModule* module)
{
  // Pull the name and superclass off the stack.
  Value name = this->fiber->stackTop[-2];
  Value superclass = this->fiber->stackTop[-1];

  // We have two values on the stack and we are going to leave one, so discard
  // the other slot.
  this->fiber->stackTop--;

  this->fiber->error = ValidateSuperclass(name, superclass, numFields);
  if (wrenHasError(this->fiber)) return;

  ObjClass* classObj = NewClass( AS_CLASS(superclass), numFields,
                                    AS_STRING(name));
  this->fiber->stackTop[-1] = OBJ_VAL(classObj);

  if (numFields == -1) NewForeign(classObj, module);
}

void WrenVM::CreateForeign(ObjFiber* fiber, Value* stack)
{
  ObjClass* classObj = AS_CLASS(stack[0]);
  ASSERT(classObj->numFields == -1, "Class must be a foreign class.");

  // TODO: Don't look up every time.
  int symbol = this->methodNames.find( "<allocate>", 10);
  ASSERT(symbol != -1, "Should have defined <allocate> symbol.");

  ASSERT(classObj->methods.size() > symbol, "Class should have allocator.");
  Method* method = &classObj->methods[symbol];
  ASSERT(method->type == METHOD_FOREIGN, "Allocator should be foreign.");

  // Pass the constructor arguments to the allocator as well.
  this->apiStack = stack;

  method->as.foreign(this);

  // End any nested calls the allocator left open, and put this fiber back in
  // charge, since a nested call may have changed the current fiber. A fiber
  // suspended in a foreign call can never be resumed, so its stack cannot have
  // moved, meaning [stack] is still valid.
  while (this->callContexts != nullptr && this->callContexts->fiber == fiber)
  {
    wrenEndCall(this, this->callContexts);
  }
  this->fiber = fiber;
  this->apiStack = nullptr;
}

void WrenVM::FinalizeForeign(ObjForeign* foreign)
{
  // TODO: Don't look up every time.
  int symbol = this->methodNames.find( "<finalize>", 10);
  ASSERT(symbol != -1, "Should have defined <finalize> symbol.");

  // If there are no finalizers, don't finalize it.
  if (symbol == -1) return;

  // If the class doesn't have a finalizer, bail out.
  ObjClass* classObj = foreign->classObj;
  if (symbol >= classObj->methods.size()) return;

  Method* method = &classObj->methods[symbol];
  if (method->type == METHOD_NONE) return;

  ASSERT(method->type == METHOD_FOREIGN, "Finalizer should be foreign.");

  WrenFinalizerFn finalizer = (WrenFinalizerFn)method->as.foreign;
  finalizer(foreign->data);
}

// Lets the host resolve an imported module name if it wants to.
Value WrenVM::ResolveModuleName(Value name)
{
  ObjFiber* fiber = this->fiber;
  ObjFn* fn = fiber->frames[fiber->numFrames - 1].closure->fn;
  ObjString* importer = fn->module->name;

  std::optional<std::string> resolved = this->ResolveModule(
      importer != nullptr
          ? std::string_view(importer->value, importer->length)
          : std::string_view(),
      std::string_view(AS_STRING(name)->value, AS_STRING(name)->length));
  if (!resolved)
  {
    this->fiber->error = StringFormat( 
        "Could not resolve module '@' imported from '@'.",
        name, OBJ_VAL(importer));
    return NULL_VAL;
  }

  // If they resolved to the exact same string, we don't need to copy it.
  if (*resolved == std::string_view(AS_STRING(name)->value,
                                    AS_STRING(name)->length))
  {
    return name;
  }

  // Copy the string into a Wren String object.
  return NewStringLength( resolved->data(), resolved->size());
}

Value WrenVM::ImportModule(Value name)
{
  name = ResolveModuleName(name);
  if (IS_NULL(name)) return NULL_VAL;

  // If the module is already loaded, we don't need to do anything.
  Value existing = wrenMapGet(this->modules, name);
  if (!IS_UNDEFINED(existing)) return existing;

  PushRoot(AS_OBJ(name));

  ObjString* nameString = AS_STRING(name);

  // Let the host try to provide the module.
  std::string source = this->LoadModule(
      std::string_view(nameString->value, nameString->length));

  // If the host didn't provide it, see if it's a built in optional module.
  if (source.empty())
  {
#if WREN_OPT_META
    if (strcmp(nameString->value, "meta") == 0) source = wrenMetaSource();
#endif
#if WREN_OPT_RANDOM
    if (strcmp(nameString->value, "random") == 0) source = wrenRandomSource();
#endif
  }

  if (source.empty())
  {
    this->fiber->error = StringFormat( "Could not load module '@'.", name);
    PopRoot(); // name.
    return NULL_VAL;
  }

  ObjClosure* moduleClosure = CompileInModule(name, source.c_str(), false, true);

  if (moduleClosure == nullptr)
  {
    this->fiber->error = StringFormat( 
                                        "Could not compile module '@'.", name);
    PopRoot(); // name.
    return NULL_VAL;
  }

  PopRoot(); // name.

  // Return the closure that executes the module.
  return OBJ_VAL(moduleClosure);
}

Value WrenVM::GetModuleVariable(ObjModule* module,
                               Value variableName)
{
  ObjString* variable = AS_STRING(variableName);
  uint32_t variableEntry = module->variableNames.find(
                                               variable->value,
                                               variable->length);
  
  // It's a runtime error if the imported variable does not exist.
  if (variableEntry != UINT32_MAX)
  {
    return module->variables[variableEntry];
  }
  
  this->fiber->error = StringFormat( 
      "Could not find a variable named '@' in module '@'.",
      variableName, OBJ_VAL(module->name));
  return NULL_VAL;
}

bool WrenVM::CheckArity(Value value, int numArgs)
{
  ASSERT(IS_CLOSURE(value), "Receiver must be a closure.");
  ObjFn* fn = AS_CLOSURE(value)->fn;

  // We only care about missing arguments, not extras. The "- 1" is because
  // numArgs includes the receiver, the function itself, which we don't want to
  // count.
  if (numArgs - 1 >= fn->arity) return true;

  this->fiber->error = CONST_STRING(this, "Function expects more arguments.");
  return false;
}


// The main bytecode interpreter loop. This is where the magic happens. It is
// also, as you can imagine, highly performance critical.
WrenInterpretResult WrenVM::RunInterpreter(ObjFiber* fiber)
{
  // Remember the current fiber so we can find it if a GC happens.
  this->fiber = fiber;
  fiber->state = FIBER_ROOT;

  // Hoist these into local variables. They are accessed frequently in the loop
  // but assigned less frequently. Keeping them in locals and updating them when
  // a call frame has been pushed or popped gives a large speed boost.
  CallFrame* frame;
  Value* stackStart;
  uint8_t* ip;
  ObjFn* fn;

  // These macros are designed to only be invoked within this function.
  #define PUSH(value)  (*fiber->stackTop++ = value)
  #define POP()        (*(--fiber->stackTop))
  #define DROP()       (fiber->stackTop--)
  #define PEEK()       (*(fiber->stackTop - 1))
  #define PEEK2()      (*(fiber->stackTop - 2))
  #define READ_BYTE()  (*ip++)
  #define READ_SHORT() (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))

  // Use this before a CallFrame is pushed to store the local variables back
  // into the current one.
  #define STORE_FRAME() frame->ip = ip

  // Use this after a CallFrame has been pushed or popped to refresh the local
  // variables.
  #define LOAD_FRAME()                                                         \
      do                                                                       \
      {                                                                        \
        frame = &fiber->frames[fiber->numFrames - 1];                          \
        stackStart = frame->stackStart;                                        \
        ip = frame->ip;                                                        \
        fn = frame->closure->fn;                                               \
      } while (false)

  // Terminates the current fiber with error string [error]. If another calling
  // fiber is willing to catch the error, transfers control to it, otherwise
  // exits the interpreter.
  #define RUNTIME_ERROR()                                                      \
      do                                                                       \
      {                                                                        \
        STORE_FRAME();                                                         \
        RuntimeError();                                                      \
        if (this->fiber == nullptr) return WREN_RESULT_RUNTIME_ERROR;               \
        fiber = this->fiber;                                                     \
        LOAD_FRAME();                                                          \
        DISPATCH();                                                            \
      } while (false)

  #if WREN_DEBUG_TRACE_INSTRUCTIONS
    // Prints the stack and instruction before each instruction is executed.
    #define DEBUG_TRACE_INSTRUCTIONS()                                         \
        do                                                                     \
        {                                                                      \
          wrenDumpStack(fiber);                                                \
          wrenDumpInstruction(this, fn, (int)(ip - fn->code.data()));              \
        } while (false)
  #else
    #define DEBUG_TRACE_INSTRUCTIONS() do { } while (false)
  #endif

  // Gives the debug hook (if one is installed) a chance to inspect the current
  // source line before the instruction at [ip] executes. Follows the same
  // discipline as a foreign method call: store the cached frame state first,
  // then run the hook -- during which API slots are wired up and a GC may
  // occur -- then refresh the cached state afterwards.
  #define DEBUG_HOOK()                                                         \
      do                                                                       \
      {                                                                        \
        if (this->debugHook != nullptr)                                          \
        {                                                                      \
          STORE_FRAME();                                                       \
          wrenVmDebugHook(this, fiber, frame);                                   \
          LOAD_FRAME();                                                        \
        }                                                                      \
      } while (false)

  #if WREN_COMPUTED_GOTO

  static void* dispatchTable[] = {
    #define OPCODE(name, _) &&code_##name,
    #include "wren_opcodes.h"
    #undef OPCODE
  };

  #define INTERPRET_LOOP    DISPATCH();
  #define CASE_CODE(name)   code_##name

  #define DISPATCH()                                                           \
      do                                                                       \
      {                                                                        \
        DEBUG_TRACE_INSTRUCTIONS();                                            \
        DEBUG_HOOK();                                                          \
        goto *dispatchTable[instruction = (Code)READ_BYTE()];                  \
      } while (false)

  #else

  #define INTERPRET_LOOP                                                       \
      loop:                                                                    \
        DEBUG_TRACE_INSTRUCTIONS();                                            \
        DEBUG_HOOK();                                                          \
        switch (instruction = (Code)READ_BYTE())

  #define CASE_CODE(name)  case CODE_##name
  #define DISPATCH()       goto loop

  #endif

  LOAD_FRAME();

  Code instruction;
  INTERPRET_LOOP
  {
    CASE_CODE(LOAD_LOCAL_0):
    CASE_CODE(LOAD_LOCAL_1):
    CASE_CODE(LOAD_LOCAL_2):
    CASE_CODE(LOAD_LOCAL_3):
    CASE_CODE(LOAD_LOCAL_4):
    CASE_CODE(LOAD_LOCAL_5):
    CASE_CODE(LOAD_LOCAL_6):
    CASE_CODE(LOAD_LOCAL_7):
    CASE_CODE(LOAD_LOCAL_8):
      PUSH(stackStart[instruction - CODE_LOAD_LOCAL_0]);
      DISPATCH();

    CASE_CODE(LOAD_LOCAL):
      PUSH(stackStart[READ_BYTE()]);
      DISPATCH();

    CASE_CODE(LOAD_FIELD_THIS):
    {
      uint8_t field = READ_BYTE();
      Value receiver = stackStart[0];
      ASSERT(IS_INSTANCE(receiver), "Receiver should be instance.");
      ObjInstance* instance = AS_INSTANCE(receiver);
      ASSERT(field < instance->classObj->numFields, "Out of bounds field.");
      PUSH(instance->fields[field]);
      DISPATCH();
    }

    CASE_CODE(POP):   DROP(); DISPATCH();
    CASE_CODE(NULL):  PUSH(NULL_VAL); DISPATCH();
    CASE_CODE(FALSE): PUSH(FALSE_VAL); DISPATCH();
    CASE_CODE(TRUE):  PUSH(TRUE_VAL); DISPATCH();

    CASE_CODE(STORE_LOCAL):
      stackStart[READ_BYTE()] = PEEK();
      DISPATCH();

    CASE_CODE(CONSTANT):
      PUSH(fn->constants[READ_SHORT()]);
      DISPATCH();

    {
      // The opcodes for doing method and superclass calls share a lot of code.
      // However, doing an if() test in the middle of the instruction sequence
      // to handle the bit that is special to super calls makes the non-super
      // call path noticeably slower.
      //
      // Instead, we do this old school using an explicit goto to share code for
      // everything at the tail end of the call-handling code that is the same
      // between normal and superclass calls.
      int numArgs;
      int symbol;

      Value* args;
      ObjClass* classObj;

      Method* method;

    CASE_CODE(CALL_0):
    CASE_CODE(CALL_1):
    CASE_CODE(CALL_2):
    CASE_CODE(CALL_3):
    CASE_CODE(CALL_4):
    CASE_CODE(CALL_5):
    CASE_CODE(CALL_6):
    CASE_CODE(CALL_7):
    CASE_CODE(CALL_8):
    CASE_CODE(CALL_9):
    CASE_CODE(CALL_10):
    CASE_CODE(CALL_11):
    CASE_CODE(CALL_12):
    CASE_CODE(CALL_13):
    CASE_CODE(CALL_14):
    CASE_CODE(CALL_15):
    CASE_CODE(CALL_16):
      // Add one for the implicit receiver argument.
      numArgs = instruction - CODE_CALL_0 + 1;
      symbol = READ_SHORT();

      // The receiver is the first argument.
      args = fiber->stackTop - numArgs;
      classObj = GetClassInline(args[0]);
      goto completeCall;

    CASE_CODE(SUPER_0):
    CASE_CODE(SUPER_1):
    CASE_CODE(SUPER_2):
    CASE_CODE(SUPER_3):
    CASE_CODE(SUPER_4):
    CASE_CODE(SUPER_5):
    CASE_CODE(SUPER_6):
    CASE_CODE(SUPER_7):
    CASE_CODE(SUPER_8):
    CASE_CODE(SUPER_9):
    CASE_CODE(SUPER_10):
    CASE_CODE(SUPER_11):
    CASE_CODE(SUPER_12):
    CASE_CODE(SUPER_13):
    CASE_CODE(SUPER_14):
    CASE_CODE(SUPER_15):
    CASE_CODE(SUPER_16):
      // Add one for the implicit receiver argument.
      numArgs = instruction - CODE_SUPER_0 + 1;
      symbol = READ_SHORT();

      // The receiver is the first argument.
      args = fiber->stackTop - numArgs;

      // The superclass is stored in a constant.
      classObj = AS_CLASS(fn->constants[READ_SHORT()]);
      goto completeCall;

    completeCall:
      // If the class's method table doesn't include the symbol, bail.
      if (symbol >= classObj->methods.size() ||
          (method = &classObj->methods[symbol])->type == METHOD_NONE)
      {
        MethodNotFound(classObj, symbol);
        RUNTIME_ERROR();
      }

      switch (method->type)
      {
        case METHOD_PRIMITIVE:
          if (method->as.primitive(this, args))
          {
            // The result is now in the first arg slot. Discard the other
            // stack slots.
            fiber->stackTop -= numArgs - 1;
          } else {
            // An error, fiber switch, or call frame change occurred.
            STORE_FRAME();

            // If we don't have a fiber to switch to, stop interpreting.
            fiber = this->fiber;
            if (fiber == nullptr) return WREN_RESULT_SUCCESS;
            if (wrenHasError(fiber)) RUNTIME_ERROR();

            // If the fiber is complete, it exited with a value using
            // Fiber.exit() and had no caller to return to, so end it.
            if (fiber->numFrames == 0) return WREN_RESULT_SUCCESS;
            LOAD_FRAME();
          }
          break;

        case METHOD_FUNCTION_CALL: 
          if (!CheckArity(args[0], numArgs)) {
            RUNTIME_ERROR();
            break;
          }

          STORE_FRAME();
          method->as.primitive(this, args);
          LOAD_FRAME();
          break;

        case METHOD_FOREIGN:
          // A foreign method may call wrenEnsureSlots(), which can grow (and
          // reallocate) the fiber's value stack. Store the cached frame state
          // first and refresh it afterwards, like the other call types do, so
          // the interpreter's local stack pointers aren't left dangling when
          // the old stack buffer is freed.
          STORE_FRAME();
          CallForeign(fiber, method->as.foreign, numArgs);
          if (wrenHasError(fiber)) RUNTIME_ERROR();
          LOAD_FRAME();
          break;

        case METHOD_BLOCK:
          STORE_FRAME();
          CallFunction(fiber, (ObjClosure*)method->as.closure, numArgs);
          LOAD_FRAME();
          break;

        case METHOD_NONE:
          UNREACHABLE();
          break;
      }
      DISPATCH();
    }

    CASE_CODE(LOAD_UPVALUE):
    {
      ObjUpvalue** upvalues = frame->closure->upvalues;
      PUSH(*upvalues[READ_BYTE()]->value);
      DISPATCH();
    }

    CASE_CODE(STORE_UPVALUE):
    {
      ObjUpvalue** upvalues = frame->closure->upvalues;
      *upvalues[READ_BYTE()]->value = PEEK();
      DISPATCH();
    }

    CASE_CODE(LOAD_MODULE_VAR):
      PUSH(fn->module->variables[READ_SHORT()]);
      DISPATCH();

    CASE_CODE(STORE_MODULE_VAR):
      fn->module->variables[READ_SHORT()] = PEEK();
      DISPATCH();

    CASE_CODE(STORE_FIELD_THIS):
    {
      uint8_t field = READ_BYTE();
      Value receiver = stackStart[0];
      ASSERT(IS_INSTANCE(receiver), "Receiver should be instance.");
      ObjInstance* instance = AS_INSTANCE(receiver);
      ASSERT(field < instance->classObj->numFields, "Out of bounds field.");
      instance->fields[field] = PEEK();
      DISPATCH();
    }

    CASE_CODE(LOAD_FIELD):
    {
      uint8_t field = READ_BYTE();
      Value receiver = POP();
      ASSERT(IS_INSTANCE(receiver), "Receiver should be instance.");
      ObjInstance* instance = AS_INSTANCE(receiver);
      ASSERT(field < instance->classObj->numFields, "Out of bounds field.");
      PUSH(instance->fields[field]);
      DISPATCH();
    }

    CASE_CODE(STORE_FIELD):
    {
      uint8_t field = READ_BYTE();
      Value receiver = POP();
      ASSERT(IS_INSTANCE(receiver), "Receiver should be instance.");
      ObjInstance* instance = AS_INSTANCE(receiver);
      ASSERT(field < instance->classObj->numFields, "Out of bounds field.");
      instance->fields[field] = PEEK();
      DISPATCH();
    }

    CASE_CODE(JUMP):
    {
      uint16_t offset = READ_SHORT();
      ip += offset;
      DISPATCH();
    }

    CASE_CODE(LOOP):
    {
      // Jump back to the top of the loop.
      uint16_t offset = READ_SHORT();
      ip -= offset;
      DISPATCH();
    }

    CASE_CODE(JUMP_IF):
    {
      uint16_t offset = READ_SHORT();
      Value condition = POP();

      if (wrenIsFalsyValue(condition)) ip += offset;
      DISPATCH();
    }

    CASE_CODE(AND):
    {
      uint16_t offset = READ_SHORT();
      Value condition = PEEK();

      if (wrenIsFalsyValue(condition))
      {
        // Short-circuit the right hand side.
        ip += offset;
      }
      else
      {
        // Discard the condition and evaluate the right hand side.
        DROP();
      }
      DISPATCH();
    }

    CASE_CODE(OR):
    {
      uint16_t offset = READ_SHORT();
      Value condition = PEEK();

      if (wrenIsFalsyValue(condition))
      {
        // Discard the condition and evaluate the right hand side.
        DROP();
      }
      else
      {
        // Short-circuit the right hand side.
        ip += offset;
      }
      DISPATCH();
    }

    CASE_CODE(CLOSE_UPVALUE):
      // Close the upvalue for the local if we have one.
      wrenCloseUpvalues(fiber, fiber->stackTop - 1);
      DROP();
      DISPATCH();

    CASE_CODE(RETURN):
    {
      Value result = POP();
      fiber->numFrames--;

      // Close any upvalues still in scope.
      wrenCloseUpvalues(fiber, stackStart);

      // If the fiber is complete, end it.
      if (fiber->numFrames == 0)
      {
        // See if there's another fiber to return to. If not, we're done.
        if (fiber->caller == nullptr)
        {
          // Store the final result value at the beginning of the stack so the
          // C API can get it.
          fiber->stack[0] = result;
          fiber->stackTop = fiber->stack + 1;
          return WREN_RESULT_SUCCESS;
        }
        
        ObjFiber* resumingFiber = fiber->caller;
        fiber->caller = nullptr;
        fiber = resumingFiber;
        this->fiber = resumingFiber;
        
        // Store the result in the resuming fiber.
        fiber->stackTop[-1] = result;
      }
      else
      {
        // Store the result of the block in the first slot, which is where the
        // caller expects it.
        stackStart[0] = result;

        // Discard the stack slots for the call frame (leaving one slot for the
        // result).
        fiber->stackTop = frame->stackStart + 1;
      }
      
      LOAD_FRAME();
      DISPATCH();
    }

    CASE_CODE(CONSTRUCT):
    {
      ASSERT(IS_CLASS(stackStart[0]), "'this' should be a class.");
      ObjClass* classObj = AS_CLASS(stackStart[0]);
      stackStart[0] = OBJ_VAL(wrenConstruct<ObjInstance>(
          this, sizeof(Value) * classObj->numFields, classObj));
      DISPATCH();
    }

    CASE_CODE(FOREIGN_CONSTRUCT):
      ASSERT(IS_CLASS(stackStart[0]), "'this' should be a class.");
      // A foreign allocator may call wrenEnsureSlots(), which can grow (and
      // reallocate) the fiber's value stack. Store the cached frame state
      // first and refresh it afterwards so the interpreter's local stack
      // pointers aren't left dangling when the old stack buffer is freed.
      STORE_FRAME();
      CreateForeign(fiber, stackStart);
      if (wrenHasError(fiber)) RUNTIME_ERROR();
      LOAD_FRAME();
      DISPATCH();

    CASE_CODE(CLOSURE):
    {
      // Create the closure and push it on the stack before creating upvalues
      // so that it doesn't get collected.
      ObjFn* function = AS_FN(fn->constants[READ_SHORT()]);
      ObjClosure* closure = wrenConstruct<ObjClosure>(
          this, sizeof(ObjUpvalue*) * function->numUpvalues, function);
      PUSH(OBJ_VAL(closure));

      // Capture upvalues, if any.
      for (int i = 0; i < function->numUpvalues; i++)
      {
        uint8_t isLocal = READ_BYTE();
        uint8_t index = READ_BYTE();
        if (isLocal)
        {
          // Make an new upvalue to close over the parent's local variable.
          closure->upvalues[i] = CaptureUpvalue(fiber,
                                                frame->stackStart + index);
        }
        else
        {
          // Use the same upvalue as the current call frame.
          closure->upvalues[i] = frame->closure->upvalues[index];
        }
      }
      DISPATCH();
    }

    CASE_CODE(END_CLASS):
    {
      EndClass();
      if (wrenHasError(fiber)) RUNTIME_ERROR();
      DISPATCH();
    }

    CASE_CODE(CLASS):
    {
      CreateClass(READ_BYTE(), nullptr);
      if (wrenHasError(fiber)) RUNTIME_ERROR();
      DISPATCH();
    }

    CASE_CODE(FOREIGN_CLASS):
    {
      CreateClass(-1, fn->module);
      if (wrenHasError(fiber)) RUNTIME_ERROR();
      DISPATCH();
    }

    CASE_CODE(METHOD_INSTANCE):
    CASE_CODE(METHOD_STATIC):
    {
      uint16_t symbol = READ_SHORT();
      ObjClass* classObj = AS_CLASS(PEEK());
      Value method = PEEK2();
      BindMethod(instruction, symbol, fn->module, classObj, method);
      if (wrenHasError(fiber)) RUNTIME_ERROR();
      DROP();
      DROP();
      DISPATCH();
    }
    
    CASE_CODE(END_MODULE):
    {
      this->lastModule = fn->module;
      PUSH(NULL_VAL);
      DISPATCH();
    }
    
    CASE_CODE(IMPORT_MODULE):
    {
      // Make a slot on the stack for the module's fiber to place the return
      // value. It will be popped after this fiber is resumed. Store the
      // imported module's closure in the slot in case a GC happens when
      // invoking the closure.
      PUSH(ImportModule(fn->constants[READ_SHORT()]));
      if (wrenHasError(fiber)) RUNTIME_ERROR();
      
      // If we get a closure, call it to execute the module body.
      if (IS_CLOSURE(PEEK()))
      {
        STORE_FRAME();
        ObjClosure* closure = AS_CLOSURE(PEEK());
        CallFunction(fiber, closure, 1);
        LOAD_FRAME();
      }
      else
      {
        // The module has already been loaded. Remember it so we can import
        // variables from it if needed.
        this->lastModule = AS_MODULE(PEEK());
      }

      DISPATCH();
    }
    
    CASE_CODE(IMPORT_VARIABLE):
    {
      Value variable = fn->constants[READ_SHORT()];
      ASSERT(this->lastModule != nullptr, "Should have already imported module.");
      Value result = GetModuleVariable(this->lastModule, variable);
      if (wrenHasError(fiber)) RUNTIME_ERROR();

      PUSH(result);
      DISPATCH();
    }

    CASE_CODE(END):
      // A CODE_END should always be preceded by a CODE_RETURN. If we get here,
      // the compiler generated wrong code.
      UNREACHABLE();
  }

  // We should only exit this function from an explicit return from CODE_RETURN
  // or a runtime error.
  UNREACHABLE();
  return WREN_RESULT_RUNTIME_ERROR;

  #undef READ_BYTE
  #undef READ_SHORT
}

WrenHandle* WrenVM::MakeCallHandle(const char* signature)
{
  ASSERT(signature != nullptr, "Signature cannot be NULL.");
  
  int signatureLength = (int)strlen(signature);
  ASSERT(signatureLength > 0, "Signature cannot be empty.");
  
  // Count the number parameters the method expects.
  int numParams = 0;
  if (signature[signatureLength - 1] == ')')
  {
    for (int i = signatureLength - 1; i > 0 && signature[i] != '('; i--)
    {
      if (signature[i] == '_') numParams++;
    }
  }
  
  // Count subscript arguments.
  if (signature[0] == '[')
  {
    for (int i = 0; i < signatureLength && signature[i] != ']'; i++)
    {
      if (signature[i] == '_') numParams++;
    }
  }
  
  // Add the signatue to the method table.
  int method =  this->methodNames.ensure(this, 
                                      signature, signatureLength);
  ASSERT(method <= MAX_METHODS, "Method limit reached.");
  
  // Create a little stub function that assumes the arguments are on the stack
  // and calls the method.
  FnDebug* debug = wrenConstruct<FnDebug>(this, 0);
  ObjFn* fn = wrenConstruct<ObjFn>(this, 0, nullptr, numParams + 1, debug);
  
  // Wrap the function in a closure and then in a handle. Do this here so it
  // doesn't get collected as we fill it in.
  WrenHandle* value = MakeHandle(OBJ_VAL(fn));
  value->value = OBJ_VAL(wrenConstruct<ObjClosure>(
      this, sizeof(ObjUpvalue*) * fn->numUpvalues, fn));
  
  fn->code.push_back((uint8_t)(CODE_CALL_0 + numParams));
  fn->code.push_back((method >> 8) & 0xff);
  fn->code.push_back(method & 0xff);
  fn->code.push_back(CODE_RETURN);
  fn->code.push_back(CODE_END);
  fn->debug->sourceLines.insert(fn->debug->sourceLines.end(), 5, 0);
  FunctionBindName( fn, signature, signatureLength);

  return value;
}

WrenInterpretResult WrenVM::Call(WrenHandle* method)
{
  ASSERT(method != nullptr, "Method cannot be NULL.");
  ASSERT(IS_CLOSURE(method->value), "Method must be a method handle.");
  ASSERT(this->fiber != nullptr, "Must set up arguments for call first.");
  ASSERT(this->apiStack != nullptr, "Must set up arguments for call first.");
  ASSERT(this->fiber->numFrames == 0, "Can not call from a foreign method.");
  
  ObjClosure* closure = AS_CLOSURE(method->value);
  
  ASSERT(this->fiber->stackTop - this->fiber->stack >= closure->fn->arity,
         "Stack must have enough arguments for method.");
  
  // Clear the API stack. Now that wrenCall() has control, we no longer need
  // it. All that matters is that the one return value is available at the end.
  this->apiStack = nullptr;

  // Discard any extra temporary slots. We take for granted that the stub
  // function has exactly one slot for each argument.
  this->fiber->stackTop = &this->fiber->stack[closure->fn->maxSlots];
  
  CallFunction(this->fiber, closure, 0);
  WrenInterpretResult result = RunInterpreter(this->fiber);
  
  // If the call didn't abort, then set up the API stack to point to the
  // beginning of the stack so the host can access the call's return value.
  if (this->fiber != nullptr) this->apiStack = this->fiber->stack;
  
  return result;
}

WrenCallContext::WrenCallContext(WrenVM* vm)
{
  fiber = vm->fiber;
  apiStackBase = (int)(vm->apiStack - vm->fiber->stack);
  prev = vm->callContexts;
  vm->callContexts = this;
}

WrenHandle::WrenHandle(WrenVM* vm, Value _value)
{
  value = _value;

  // Link the handle into the front of the VM's list of handles.
  prev = nullptr;
  next = vm->handles;
  if (vm->handles != nullptr) vm->handles->prev = this;
  vm->handles = this;
}

WrenCallContext* WrenVM::BeginCall()
{
  ASSERT(this->apiStack != nullptr,
         "Must be in a foreign method (or have set up slots) to begin a call.");

  // Remember the current slot region so that it can be restored when the
  // nested call is done. The base is stored as an index into the fiber's
  // stack because the nested code may reallocate and move the stack of any
  // other fiber -- and because this fiber is blocked in the foreign method
  // for the duration, this is the only chance we have to record it.
  WrenCallContext* context =
      new (ALLOCATE(this, WrenCallContext)) WrenCallContext(this);

  // Create a fresh fiber to run the nested call in. Giving each nested call
  // its own fiber ensures that a Wren stack only ever has a single foreign
  // API call at the top of it, and means the nested call's slots start empty
  // at the bottom of the fiber's stack, just like a non-nested one.
  //
  // The context links the suspended fiber into the VM so that the garbage
  // collector can find it and so that Wren code can detect that it can't be
  // resumed while the foreign method is still running.
  ObjFiber* fiber = wrenConstruct<ObjFiber>(this, 0, nullptr);
  this->fiber = fiber;
  this->apiStack = fiber->stack;

  return context;
}

void WrenVM::EndCall(WrenCallContext* context)
{
  ASSERT(context != nullptr, "Context cannot be NULL.");
  ASSERT(this->callContexts == context,
         "Must end nested calls in the reverse order they were begun.");

  // Unlink the context from the stack of nested calls.
  this->callContexts = context->prev;

  // Restore the suspended fiber and its slot region. Since the fiber could
  // not be resumed while it was suspended in the foreign method, its stack
  // cannot have moved, so the saved base index still points to the same slot.
  this->fiber = context->fiber;
  this->apiStack = context->fiber->stack + context->apiStackBase;

  DEALLOCATE(this, context);
}

WrenHandle* WrenVM::MakeHandle(Value value)
{
  if (IS_OBJ(value)) PushRoot(AS_OBJ(value));

  // Make a handle for it. The constructor links it into the VM's list.
  WrenHandle* handle =
      new (ALLOCATE(this, WrenHandle)) WrenHandle(this, value);

  if (IS_OBJ(value)) PopRoot();

  return handle;
}

void WrenVM::ReleaseHandle(WrenHandle* handle)
{
  ASSERT(handle != nullptr, "Handle cannot be NULL.");

  // Update the VM's head pointer if we're releasing the first handle.
  if (this->handles == handle) this->handles = handle->next;

  // Unlink it from the list.
  if (handle->prev != nullptr) handle->prev->next = handle->next;
  if (handle->next != nullptr) handle->next->prev = handle->prev;

  // Clear it out. This isn't strictly necessary since we're going to free it,
  // but it makes for easier debugging.
  handle->prev = nullptr;
  handle->next = nullptr;
  handle->value = NULL_VAL;
  DEALLOCATE(this, handle);
}

WrenInterpretResult WrenVM::InterpretSource(const char* module,
                                  const char* source)
{
  ObjClosure* closure = CompileSource(module, source, false, true);
  if (closure == nullptr) return WREN_RESULT_COMPILE_ERROR;
  
  PushRoot((Obj*)closure);
  ObjFiber* fiber = wrenConstruct<ObjFiber>(this, 0, closure);
  PopRoot(); // closure.

  // If this is a re-entrant call from inside a foreign method, suspend the
  // foreign method's slots for the duration. wrenInterpret() doesn't use the
  // API slots itself, but suspending them keeps the foreign method's fiber
  // alive and prevents Wren code from resuming it while the source runs.
  WrenCallContext* context =
      this->apiStack != nullptr ? BeginCall() : nullptr;
  this->apiStack = nullptr;

  WrenInterpretResult result = RunInterpreter(fiber);

  if (context != nullptr) EndCall(context);

  return result;
}

ObjClosure* WrenVM::CompileSource(const char* module, const char* source,
                            bool isExpression, bool printErrors)
{
  Value nameValue = NULL_VAL;
  if (module != nullptr)
  {
    nameValue = NewString( module);
    PushRoot(AS_OBJ(nameValue));
  }
  
  ObjClosure* closure = CompileInModule(nameValue, source,
                                        isExpression, printErrors);

  if (module != nullptr) PopRoot(); // nameValue.
  return closure;
}

Value WrenVM::GetModuleVariable(Value moduleName, Value variableName)
{
  ObjModule* module = GetModule(moduleName);
  if (module == nullptr)
  {
    this->fiber->error = StringFormat( "Module '@' is not loaded.",
                                        moduleName);
    return NULL_VAL;
  }
  
  return GetModuleVariable(module, variableName);
}

Value WrenVM::FindVariable(ObjModule* module, const char* name)
{
  int symbol = module->variableNames.find( name, strlen(name));
  return module->variables[symbol];
}

int WrenVM::DeclareVariable(ObjModule* module, const char* name,
                        size_t length, int line)
{
  if (module->variables.size() == MAX_MODULE_VARS) return -2;

  // Implicitly defined variables get a "value" that is the line where the
  // variable is first used. We'll use that later to report an error on the
  // right line.
  module->variables.push_back(NUM_VAL(line));
  return module->variableNames.add(this, name, length);
}

int WrenVM::DefineVariable(ObjModule* module, const char* name,
                       size_t length, Value value, int* line)
{
  if (module->variables.size() == MAX_MODULE_VARS) return -2;

  if (IS_OBJ(value)) PushRoot(AS_OBJ(value));

  // See if the variable is already explicitly or implicitly declared.
  int symbol = module->variableNames.find( name, length);

  if (symbol == -1)
  {
    // Brand new variable.
    symbol = module->variableNames.add(this, name, length);
    module->variables.push_back(value);
  }
  else if (IS_NUM(module->variables[symbol]))
  {
    // An implicitly declared variable's value will always be a number.
    // Now we have a real definition.
    if(line) *line = (int)AS_NUM(module->variables[symbol]);
    module->variables[symbol] = value;

	// If this was a localname we want to error if it was 
	// referenced before this definition.
	if (wrenIsLocalName(name)) symbol = -3;
  }
  else
  {
    // Already explicitly declared.
    symbol = -1;
  }

  if (IS_OBJ(value)) PopRoot();

  return symbol;
}

// TODO: Inline?
void WrenVM::PushRoot(Obj* obj)
{
  ASSERT(obj != nullptr, "Can't root NULL.");
  ASSERT(this->numTempRoots < WREN_MAX_TEMP_ROOTS, "Too many temporary roots.");

  this->tempRoots[this->numTempRoots++] = obj;
}

void WrenVM::PopRoot()
{
  ASSERT(this->numTempRoots > 0, "No temporary roots to release.");
  this->numTempRoots--;
}

int WrenVM::SlotCount() const
{
  if (this->apiStack == nullptr) return 0;
  
  return (int)(this->fiber->stackTop - this->apiStack);
}

void WrenVM::EnsureSlots(int numSlots)
{
  // If we don't have a fiber accessible, create one for the API to use.
  if (this->apiStack == nullptr)
  {
    this->fiber = wrenConstruct<ObjFiber>(this, 0, nullptr);
    this->apiStack = this->fiber->stack;
  }
  
  int currentSize = (int)(this->fiber->stackTop - this->apiStack);
  if (currentSize >= numSlots) return;
  
  // Grow the stack if needed.
  int needed = (int)(this->apiStack - this->fiber->stack) + numSlots;
  EnsureStack( this->fiber, needed);
  
  // Initialize any newly exposed slots. The memory between the old stack top
  // and the new one is uninitialized, and since the garbage collector scans
  // every slot up to stackTop, garbage there can crash the GC.
  for (Value* slot = this->fiber->stackTop; slot < this->apiStack + numSlots; slot++)
  {
    *slot = NULL_VAL;
  }
  
  this->fiber->stackTop = this->apiStack + numSlots;
}

// Ensures that [slot] is a valid index into the API's stack of slots.
void WrenVM::ValidateApiSlot(int slot) const
{
  ASSERT(slot >= 0, "Slot cannot be negative.");
  ASSERT(slot < wrenGetSlotCount(this), "Not that many slots.");
}

// Gets the type of the object in [slot].
WrenType WrenVM::SlotType(int slot) const
{
  ValidateApiSlot(slot);
  if (IS_BOOL(this->apiStack[slot])) return WREN_TYPE_BOOL;
  if (IS_NUM(this->apiStack[slot])) return WREN_TYPE_NUM;
  if (IS_FOREIGN(this->apiStack[slot])) return WREN_TYPE_FOREIGN;
  if (IS_LIST(this->apiStack[slot])) return WREN_TYPE_LIST;
  if (IS_MAP(this->apiStack[slot])) return WREN_TYPE_MAP;
  if (IS_NULL(this->apiStack[slot])) return WREN_TYPE_NULL;
  if (IS_STRING(this->apiStack[slot])) return WREN_TYPE_STRING;
  
  return WREN_TYPE_UNKNOWN;
}

bool WrenVM::GetSlotBool(int slot)
{
  ValidateApiSlot(slot);
  ASSERT(IS_BOOL(this->apiStack[slot]), "Slot must hold a bool.");

  return AS_BOOL(this->apiStack[slot]);
}

const char* WrenVM::GetSlotBytes(int slot, int* length)
{
  ValidateApiSlot(slot);
  ASSERT(IS_STRING(this->apiStack[slot]), "Slot must hold a string.");
  
  ObjString* string = AS_STRING(this->apiStack[slot]);
  *length = string->length;
  return string->value;
}

double WrenVM::GetSlotDouble(int slot)
{
  ValidateApiSlot(slot);
  ASSERT(IS_NUM(this->apiStack[slot]), "Slot must hold a number.");

  return AS_NUM(this->apiStack[slot]);
}

void* WrenVM::GetSlotForeign(int slot)
{
  ValidateApiSlot(slot);
  ASSERT(IS_FOREIGN(this->apiStack[slot]),
         "Slot must hold a foreign instance.");

  return AS_FOREIGN(this->apiStack[slot])->data;
}

const char* WrenVM::GetSlotString(int slot)
{
  ValidateApiSlot(slot);
  ASSERT(IS_STRING(this->apiStack[slot]), "Slot must hold a string.");

  return AS_CSTRING(this->apiStack[slot]);
}

WrenHandle* WrenVM::GetSlotHandle(int slot)
{
  ValidateApiSlot(slot);
  return MakeHandle(this->apiStack[slot]);
}

// Stores [value] in [slot] in the foreign call stack.
void WrenVM::SetSlot(int slot, Value value)
{
  ValidateApiSlot(slot);
  this->apiStack[slot] = value;
}

void WrenVM::SetSlotBool(int slot, bool value)
{
  SetSlot(slot, BOOL_VAL(value));
}

void WrenVM::SetSlotBytes(int slot, const char* bytes, size_t length)
{
  ASSERT(bytes != nullptr, "Byte array cannot be NULL.");
  SetSlot(slot, NewStringLength( bytes, length));
}

void WrenVM::SetSlotDouble(int slot, double value)
{
  SetSlot(slot, NUM_VAL(value));
}

void* WrenVM::SetSlotNewForeign(int slot, int classSlot, size_t size)
{
  ValidateApiSlot(slot);
  ValidateApiSlot(classSlot);
  ASSERT(IS_CLASS(this->apiStack[classSlot]), "Slot must hold a class.");
  
  ObjClass* classObj = AS_CLASS(this->apiStack[classSlot]);
  ASSERT(classObj->numFields == -1, "Class must be a foreign class.");
  
  ObjForeign* foreign = wrenConstruct<ObjForeign>(this, size, classObj, size);
  this->apiStack[slot] = OBJ_VAL(foreign);
  
  return (void*)foreign->data;
}

void WrenVM::SetSlotNewList(int slot)
{
  SetSlot(slot, OBJ_VAL(wrenConstruct<ObjList>(this, 0, 0)));
}

void WrenVM::SetSlotNewMap(int slot)
{
  SetSlot(slot, OBJ_VAL(wrenConstruct<ObjMap>(this, 0)));
}

void WrenVM::SetNull(int slot)
{
  SetSlot(slot, NULL_VAL);
}

void WrenVM::SetSlotString(int slot, const char* text)
{
  ASSERT(text != nullptr, "String cannot be NULL.");
  
  SetSlot(slot, NewString( text));
}

void WrenVM::SetSlotHandle(int slot, WrenHandle* handle)
{
  ASSERT(handle != nullptr, "Handle cannot be NULL.");

  SetSlot(slot, handle->value);
}

int WrenVM::GetListCount(int slot)
{
  ValidateApiSlot(slot);
  ASSERT(IS_LIST(this->apiStack[slot]), "Slot must hold a list.");
  
  ValueBuffer& elements = AS_LIST(this->apiStack[slot])->elements;
  return elements.size();
}

void WrenVM::GetListElement(int listSlot, int index, int elementSlot)
{
  ValidateApiSlot(listSlot);
  ValidateApiSlot(elementSlot);
  ASSERT(IS_LIST(this->apiStack[listSlot]), "Slot must hold a list.");

  ValueBuffer& elements = AS_LIST(this->apiStack[listSlot])->elements;

  uint32_t usedIndex = wrenValidateIndex(elements.size(), index);
  ASSERT(usedIndex != UINT32_MAX, "Index out of bounds.");

  this->apiStack[elementSlot] = elements[usedIndex];
}

void WrenVM::SetListElement(int listSlot, int index, int elementSlot)
{
  ValidateApiSlot(listSlot);
  ValidateApiSlot(elementSlot);
  ASSERT(IS_LIST(this->apiStack[listSlot]), "Slot must hold a list.");

  ObjList* list = AS_LIST(this->apiStack[listSlot]);

  uint32_t usedIndex = wrenValidateIndex(list->elements.size(), index);
  ASSERT(usedIndex != UINT32_MAX, "Index out of bounds.");
  
  list->elements[usedIndex] = this->apiStack[elementSlot];
}

void WrenVM::InsertInList(int listSlot, int index, int elementSlot)
{
  ValidateApiSlot(listSlot);
  ValidateApiSlot(elementSlot);
  ASSERT(IS_LIST(this->apiStack[listSlot]), "Must insert into a list.");
  
  ObjList* list = AS_LIST(this->apiStack[listSlot]);
  
  // Negative indices count from the end. 
  // We don't use wrenValidateIndex here because insert allows 1 past the end.
  if (index < 0) index = list->elements.size() + 1 + index;
  
  ASSERT(index <= list->elements.size(), "Index out of bounds.");
  
  ListInsert( list, this->apiStack[elementSlot], index);
}

int WrenVM::GetMapCount(int slot)
{
  ValidateApiSlot(slot);
  ASSERT(IS_MAP(this->apiStack[slot]), "Slot must hold a map.");

  ObjMap* map = AS_MAP(this->apiStack[slot]);
  return map->count;
}

bool WrenVM::GetMapContainsKey(int mapSlot, int keySlot)
{
  ValidateApiSlot(mapSlot);
  ValidateApiSlot(keySlot);
  ASSERT(IS_MAP(this->apiStack[mapSlot]), "Slot must hold a map.");

  Value key = this->apiStack[keySlot];
  ASSERT(wrenMapIsValidKey(key), "Key must be a value type");
  if (!validateKey(this, key)) return false;

  ObjMap* map = AS_MAP(this->apiStack[mapSlot]);
  Value value = wrenMapGet(map, key);

  return !IS_UNDEFINED(value);
}

void WrenVM::GetMapValue(int mapSlot, int keySlot, int valueSlot)
{
  ValidateApiSlot(mapSlot);
  ValidateApiSlot(keySlot);
  ValidateApiSlot(valueSlot);
  ASSERT(IS_MAP(this->apiStack[mapSlot]), "Slot must hold a map.");

  ObjMap* map = AS_MAP(this->apiStack[mapSlot]);
  Value value = wrenMapGet(map, this->apiStack[keySlot]);
  if (IS_UNDEFINED(value)) {
    value = NULL_VAL;
  }

  this->apiStack[valueSlot] = value;
}

void WrenVM::SetMapValue(int mapSlot, int keySlot, int valueSlot)
{
  ValidateApiSlot(mapSlot);
  ValidateApiSlot(keySlot);
  ValidateApiSlot(valueSlot);
  ASSERT(IS_MAP(this->apiStack[mapSlot]), "Must insert into a map.");
  
  Value key = this->apiStack[keySlot];
  ASSERT(wrenMapIsValidKey(key), "Key must be a value type");

  if (!validateKey(this, key)) {
    return;
  }

  Value value = this->apiStack[valueSlot];
  ObjMap* map = AS_MAP(this->apiStack[mapSlot]);
  
  MapSet( map, key, value);
}

void WrenVM::LookupVariable(const char* module, const char* name,
                     int slot)
{
  ASSERT(module != nullptr, "Module cannot be NULL.");
  ASSERT(name != nullptr, "Variable name cannot be NULL.");  

  Value moduleName = StringFormat( "$", module);
  PushRoot(AS_OBJ(moduleName));
  
  ObjModule* moduleObj = GetModule(moduleName);
  ASSERT(moduleObj != nullptr, "Could not find module.");
  
  PopRoot(); // moduleName.

  int variableSlot = moduleObj->variableNames.find(
                                         name, strlen(name));
  ASSERT(variableSlot != -1, "Could not find variable.");
  
  SetSlot(slot, moduleObj->variables[variableSlot]);
}

bool WrenVM::HasVariable(const char* module, const char* name)
{
  ASSERT(module != nullptr, "Module cannot be NULL.");
  ASSERT(name != nullptr, "Variable name cannot be NULL.");

  Value moduleName = StringFormat( "$", module);
  PushRoot(AS_OBJ(moduleName));

  //We don't use wrenHasModule since we want to use the module object.
  ObjModule* moduleObj = GetModule(moduleName);
  ASSERT(moduleObj != nullptr, "Could not find module.");

  PopRoot(); // moduleName.

  int variableSlot = moduleObj->variableNames.find(
    name, strlen(name));

  return variableSlot != -1;
}

bool WrenVM::HasModule(const char* module)
{
  ASSERT(module != nullptr, "Module cannot be NULL.");
  
  Value moduleName = StringFormat( "$", module);
  PushRoot(AS_OBJ(moduleName));

  ObjModule* moduleObj = GetModule(moduleName);
  
  PopRoot(); // moduleName.

  return moduleObj != nullptr;
}

void WrenVM::AbortFiber(int slot)
{
  ValidateApiSlot(slot);
  this->fiber->error = this->apiStack[slot];
}

void* WrenVM::UserData() const
{
	return this->config.userData;
}

void WrenVM::SetUserData(void* userData)
{
	this->config.userData = userData;
}

// Free forwarder for headers that precede the WrenVM class definition.
void* wrenReallocate(WrenVM* vm, void* memory, size_t oldSize, size_t newSize)
{
  return vm->Reallocate(memory, oldSize, newSize);
}

void WrenVM::RemoveMapValue(int mapSlot, int keySlot, int removedValueSlot)
{
  ValidateApiSlot(mapSlot);
  ValidateApiSlot(keySlot);
  ASSERT(IS_MAP(apiStack[mapSlot]), "Slot must hold a map.");

  Value key = apiStack[keySlot];
  if (!validateKey(this, key)) {
    return;
  }

  ObjMap* map = AS_MAP(apiStack[mapSlot]);
  Value removed = MapRemoveKey( map, key);
  SetSlot(removedValueSlot, removed);
}

// ---------------------------------------------------------------------------
// Exported C API (wren.h): one-line forwarders to the WrenVM members.
// ---------------------------------------------------------------------------

int wrenGetSlotCount(const WrenVM* vm) { return vm->SlotCount(); }
void wrenEnsureSlots(WrenVM* vm, int numSlots) { vm->EnsureSlots(numSlots); }
WrenType wrenGetSlotType(const WrenVM* vm, int slot) { return vm->SlotType(slot); }
bool wrenGetSlotBool(WrenVM* vm, int slot) { return vm->GetSlotBool(slot); }
const char* wrenGetSlotBytes(WrenVM* vm, int slot, int* length) { return vm->GetSlotBytes(slot, length); }
double wrenGetSlotDouble(WrenVM* vm, int slot) { return vm->GetSlotDouble(slot); }
void* wrenGetSlotForeign(WrenVM* vm, int slot) { return vm->GetSlotForeign(slot); }
const char* wrenGetSlotString(WrenVM* vm, int slot) { return vm->GetSlotString(slot); }
WrenHandle* wrenGetSlotHandle(WrenVM* vm, int slot) { return vm->GetSlotHandle(slot); }
void wrenSetSlotBool(WrenVM* vm, int slot, bool value) { vm->SetSlotBool(slot, value); }
void wrenSetSlotBytes(WrenVM* vm, int slot, const char* bytes, size_t length) { vm->SetSlotBytes(slot, bytes, length); }
void wrenSetSlotDouble(WrenVM* vm, int slot, double value) { vm->SetSlotDouble(slot, value); }
void* wrenSetSlotNewForeign(WrenVM* vm, int slot, int classSlot, size_t size) { return vm->SetSlotNewForeign(slot, classSlot, size); }
void wrenSetSlotNewList(WrenVM* vm, int slot) { vm->SetSlotNewList(slot); }
void wrenSetSlotNewMap(WrenVM* vm, int slot) { vm->SetSlotNewMap(slot); }
void wrenSetSlotNull(WrenVM* vm, int slot) { vm->SetNull(slot); }
void wrenSetSlotString(WrenVM* vm, int slot, const char* text) { vm->SetSlotString(slot, text); }
void wrenSetSlotHandle(WrenVM* vm, int slot, WrenHandle* handle) { vm->SetSlotHandle(slot, handle); }
int wrenGetListCount(WrenVM* vm, int slot) { return vm->GetListCount(slot); }
void wrenGetListElement(WrenVM* vm, int listSlot, int index, int elementSlot) { vm->GetListElement(listSlot, index, elementSlot); }
void wrenSetListElement(WrenVM* vm, int listSlot, int index, int elementSlot) { vm->SetListElement(listSlot, index, elementSlot); }
void wrenInsertInList(WrenVM* vm, int listSlot, int index, int elementSlot) { vm->InsertInList(listSlot, index, elementSlot); }
int wrenGetMapCount(WrenVM* vm, int slot) { return vm->GetMapCount(slot); }
bool wrenGetMapContainsKey(WrenVM* vm, int mapSlot, int keySlot) { return vm->GetMapContainsKey(mapSlot, keySlot); }
void wrenGetMapValue(WrenVM* vm, int mapSlot, int keySlot, int valueSlot) { vm->GetMapValue(mapSlot, keySlot, valueSlot); }
void wrenSetMapValue(WrenVM* vm, int mapSlot, int keySlot, int valueSlot) { vm->SetMapValue(mapSlot, keySlot, valueSlot); }
void wrenRemoveMapValue(WrenVM* vm, int mapSlot, int keySlot, int removedValueSlot) { vm->RemoveMapValue(mapSlot, keySlot, removedValueSlot); }
void wrenGetVariable(WrenVM* vm, const char* module, const char* name, int slot) { vm->LookupVariable(module, name, slot); }
bool wrenHasVariable(WrenVM* vm, const char* module, const char* name) { return vm->HasVariable(module, name); }
bool wrenHasModule(WrenVM* vm, const char* module) { return vm->HasModule(module); }
void wrenAbortFiber(WrenVM* vm, int slot) { vm->AbortFiber(slot); }
void* wrenGetUserData(const WrenVM* vm) { return vm->UserData(); }
void wrenSetUserData(WrenVM* vm, void* userData) { vm->SetUserData(userData); }
WrenInterpretResult wrenInterpret(WrenVM* vm, const char* module, const char* source) { return vm->InterpretSource(module, source); }
WrenHandle* wrenMakeCallHandle(WrenVM* vm, const char* signature) { return vm->MakeCallHandle(signature); }
WrenInterpretResult wrenCall(WrenVM* vm, WrenHandle* method) { return vm->Call(method); }
WrenCallContext* wrenBeginCall(WrenVM* vm) { return vm->BeginCall(); }
void wrenEndCall(WrenVM* vm, WrenCallContext* context) { vm->EndCall(context); }
void wrenReleaseHandle(WrenVM* vm, WrenHandle* handle) { vm->ReleaseHandle(handle); }
void wrenCollectGarbage(WrenVM* vm) { vm->CollectGarbage(); }
