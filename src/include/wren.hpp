#ifndef wren_hpp
#define wren_hpp

// Wren C++20 API — A modern, type-safe interface for the Wren scripting language.
//
// This header provides:
//   - Fluent module/class/method registration (wrenbind17/pybind11 inspired)
//   - Type-safe foreign object storage via shared_ptr wrappers
//   - RAII handles and VM lifecycle management
//   - Type-safe slot access with automatic conversions
//   - Safe cross-language object lifetime management
//
// The underlying VM (NaN-boxing, GC, bytecode) is unchanged. This layer wraps
// the raw C API with modern C++20 ergonomics.
//
// Key design: Foreign objects are stored as ForeignWrapper (shared_ptr + type tag)
// inside Wren's foreign slots. This enables:
//   - Safe shared ownership across the C++↔Wren boundary
//   - Runtime type checking when extracting foreign arguments
//   - Passing registered foreign types as method parameters
//
// Template-generated static trampoline functions serve as the C function pointers
// that Wren's VM expects. Each unique <method-pointer, Class, Args...> instantiation
// generates a unique function address, enabling zero-overhead dispatch.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
  #include "wren.h"
}

namespace wren {

class VM;
template <typename T> class Foreign;
class Module;
class Handle;

// ===========================================================================
// Registration records — stores plain function pointers
// ===========================================================================
struct MethodReg {
  WrenForeignMethodFn fn = nullptr;
  bool isStatic = false;
  std::string signature;
};

struct ClassReg {
  std::string className;
  WrenForeignMethodFn allocator = nullptr;
  WrenFinalizerFn finalizer = nullptr;
  std::vector<int> ctorArgCounts;
  std::vector<MethodReg> methods;
  std::string wrenSource;
};

struct ModuleReg {
  std::string name;
  std::vector<ClassReg> classes;
  std::string extraSource;
};

namespace detail {

// ===========================================================================
// ForeignWrapper — stored inside Wren's foreign slot for every C++20-managed
// foreign object. Contains a shared_ptr to the actual object and a type tag
// for runtime type checking.
// ===========================================================================
struct ForeignWrapper {
  std::shared_ptr<void> obj;
  const std::type_info* type = nullptr;

  ForeignWrapper() = default;

  template <typename T, typename... Args>
  static ForeignWrapper make(Args&&... args) {
    ForeignWrapper w;
    w.obj = std::make_shared<T>(std::forward<Args>(args)...);
    w.type = &typeid(T);
    return w;
  }

  // Create a wrapper that shares ownership of an existing shared_ptr
  template <typename T>
  static ForeignWrapper share(std::shared_ptr<T> ptr) {
    ForeignWrapper w;
    w.obj = std::move(ptr);
    w.type = &typeid(T);
    return w;
  }
};

// ===========================================================================
// Type converters: C++ type <-> Wren slot
// ===========================================================================
template <typename T, typename = void> struct Converter;

template <> struct Converter<double> {
  static double get(WrenVM* vm, int slot) { return wrenGetSlotDouble(vm, slot); }
  static void set(WrenVM* vm, int slot, double v) { wrenSetSlotDouble(vm, slot, v); }
};
template <> struct Converter<float> {
  static float get(WrenVM* vm, int slot) { return static_cast<float>(wrenGetSlotDouble(vm, slot)); }
  static void set(WrenVM* vm, int slot, float v) { wrenSetSlotDouble(vm, slot, static_cast<double>(v)); }
};
template <> struct Converter<bool> {
  static bool get(WrenVM* vm, int slot) { return wrenGetSlotBool(vm, slot); }
  static void set(WrenVM* vm, int slot, bool v) { wrenSetSlotBool(vm, slot, v); }
};

// Generic converter for all integer types (int, long, short, char, unsigned, etc.)
template <typename T>
struct Converter<T, std::enable_if_t<std::is_integral_v<T>>> {
  static T get(WrenVM* vm, int slot) { return static_cast<T>(wrenGetSlotDouble(vm, slot)); }
  static void set(WrenVM* vm, int slot, T v) { wrenSetSlotDouble(vm, slot, static_cast<double>(v)); }
};
template <> struct Converter<std::string> {
  static std::string get(WrenVM* vm, int slot) {
    int len = 0;
    const char* b = wrenGetSlotBytes(vm, slot, &len);
    return std::string(b, static_cast<size_t>(len));
  }
  static void set(WrenVM* vm, int slot, const std::string& v) {
    wrenSetSlotBytes(vm, slot, v.data(), v.size());
  }
};
template <> struct Converter<std::string_view> {
  static std::string_view get(WrenVM* vm, int slot) {
    int len = 0;
    const char* b = wrenGetSlotBytes(vm, slot, &len);
    return std::string_view(b, static_cast<size_t>(len));
  }
  static void set(WrenVM* vm, int slot, std::string_view v) {
    wrenSetSlotBytes(vm, slot, v.data(), v.size());
  }
};
template <> struct Converter<const char*> {
  static const char* get(WrenVM* vm, int slot) { return wrenGetSlotString(vm, slot); }
  static void set(WrenVM* vm, int slot, const char* v) { wrenSetSlotString(vm, slot, v); }
};

// Helper concept for built-in convertible types
template <typename T>
inline constexpr bool isWrenBuiltin_v =
  std::is_arithmetic_v<T> ||
  std::is_same_v<T, std::string> ||
  std::is_same_v<T, std::string_view> ||
  std::is_same_v<T, const char*>;

// ===========================================================================
// Slot access helpers for foreign (wrapper-based) objects
// ===========================================================================

// Extract a typed reference from a foreign slot containing a ForeignWrapper.
template <typename T>
inline T& getForeign(WrenVM* vm, int slot) {
  void* data = wrenGetSlotForeign(vm, slot);
  auto* wrapper = std::launder(reinterpret_cast<ForeignWrapper*>(data));
  return *static_cast<T*>(wrapper->obj.get());
}

// Extract a shared_ptr from a foreign slot — for shared ownership scenarios.
template <typename T>
inline std::shared_ptr<T> getForeignShared(WrenVM* vm, int slot) {
  void* data = wrenGetSlotForeign(vm, slot);
  auto* wrapper = std::launder(reinterpret_cast<ForeignWrapper*>(data));
  return std::static_pointer_cast<T>(wrapper->obj);
}

// Generic argument getter — handles builtins, foreign by-value/ref/ptr.
template <typename T>
inline auto getArg(WrenVM* vm, int slot) {
  using D = std::decay_t<T>;
  if constexpr (isWrenBuiltin_v<D>) {
    return Converter<D>::get(vm, slot);
  } else if constexpr (std::is_pointer_v<D>) {
    // Pointer argument: return address of the foreign object
    using PointedTo = std::remove_pointer_t<D>;
    return &getForeign<PointedTo>(vm, slot);
  } else {
    // Reference or value: return reference, caller copies if needed
    return getForeign<D>(vm, slot);
  }
}

// Place a foreign return value into slot 0, wrapping it in a ForeignWrapper.
template <typename T>
inline void setReturn(WrenVM* vm, T&& value) {
  using D = std::decay_t<T>;
  if constexpr (isWrenBuiltin_v<D>) {
    Converter<D>::set(vm, 0, std::forward<T>(value));
  } else {
    void* data = wrenSetSlotNewForeign(vm, 0, 0, sizeof(ForeignWrapper));
    new (data) ForeignWrapper(ForeignWrapper::make<D>(std::forward<T>(value)));
  }
}

// ===========================================================================
// Template-generated trampoline functions.
// Each is a unique static function that Wren's VM calls via C function pointer.
// All helper logic is inlined to avoid two-phase lookup issues.
// ===========================================================================

// --- Member function trampoline ---
template <auto Fn, typename Class, typename... Args>
void memberTrampoline(WrenVM* vm) {
  Class& obj = getForeign<Class>(vm, 0);
  if constexpr (sizeof...(Args) == 0) {
    if constexpr (std::is_void_v<std::invoke_result_t<decltype(Fn), Class&>>) {
      (obj.*Fn)();
    } else {
      setReturn(vm, (obj.*Fn)());
    }
  } else {
    // Unpack arguments: slot 0 = receiver, slots 1..N = args
    [&obj, vm]<std::size_t... Is>(std::index_sequence<Is...>) {
      if constexpr (std::is_void_v<std::invoke_result_t<decltype(Fn), Class&, Args...>>) {
        (obj.*Fn)(getArg<Args>(vm, static_cast<int>(Is) + 1)...);
      } else {
        setReturn(vm, (obj.*Fn)(getArg<Args>(vm, static_cast<int>(Is) + 1)...));
      }
    }(std::index_sequence_for<Args...>{});
  }
}

// --- Free / static function trampoline ---
template <auto Fn, typename... Args>
void freeTrampoline(WrenVM* vm) {
  if constexpr (sizeof...(Args) == 0) {
    if constexpr (std::is_void_v<std::invoke_result_t<decltype(Fn)>>) {
      Fn();
    } else {
      setReturn(vm, Fn());
    }
  } else {
    [vm]<std::size_t... Is>(std::index_sequence<Is...>) {
      if constexpr (std::is_void_v<std::invoke_result_t<decltype(Fn), Args...>>) {
        Fn(getArg<Args>(vm, static_cast<int>(Is) + 1)...);
      } else {
        setReturn(vm, Fn(getArg<Args>(vm, static_cast<int>(Is) + 1)...));
      }
    }(std::index_sequence_for<Args...>{});
  }
}

// --- Property getter trampoline ---
template <auto Getter, typename Class>
void getterTrampoline(WrenVM* vm) {
  Class& obj = getForeign<Class>(vm, 0);
  if constexpr (std::is_member_object_pointer_v<decltype(Getter)>) {
    setReturn(vm, obj.*Getter);
  } else {
    setReturn(vm, (obj.*Getter)());
  }
}

// --- Setter value-type deduction ---
// Deduce the value type written by a property setter so users don't have to
// pass it explicitly (mirrors var()'s automatic member-type deduction):
//   free/static function  R (*)(C, V)  -> V
//   member function       R (C::*)(V) -> V  (const/noexcept variants included)
//   member object         R C::*      -> R
// Unsupported setter shapes fail to compile with an incomplete-type error.
template <typename S>
struct setter_value;  // intentionally undefined

template <typename R, typename C, typename V>
struct setter_value<R (*)(C, V)> { using type = V; };
template <typename R, typename C, typename V>
struct setter_value<R (*)(C, V) noexcept> { using type = V; };
template <typename R, typename C, typename V>
struct setter_value<R (C::*)(V)> { using type = V; };
template <typename R, typename C, typename V>
struct setter_value<R (C::*)(V) const> { using type = V; };
template <typename R, typename C, typename V>
struct setter_value<R (C::*)(V) noexcept> { using type = V; };
template <typename R, typename C, typename V>
struct setter_value<R (C::*)(V) const noexcept> { using type = V; };
template <typename R, typename C>
struct setter_value<R C::*> { using type = R; };

template <typename S>
using setter_value_t = typename setter_value<S>::type;

// --- Property setter trampoline ---
template <auto Setter, typename Class, typename ValType>
void setterTrampoline(WrenVM* vm) {
  Class& obj = getForeign<Class>(vm, 0);
  if constexpr (std::is_member_object_pointer_v<decltype(Setter)>) {
    obj.*Setter = getArg<ValType>(vm, 1);
  } else {
    (obj.*Setter)(getArg<ValType>(vm, 1));
  }
}

  // --- External function trampoline (funcExt) ---
  // Calls a free function, passing the foreign object as the first argument.
  // This allows extending a class with non-member functions, like wrenbind17's funcExt.
  template <auto Fn, typename Class, typename... Args>
  void funcExtTrampoline(WrenVM* vm) {
    Class& obj = getForeign<Class>(vm, 0);
    if constexpr (sizeof...(Args) == 0) {
      if constexpr (std::is_void_v<std::invoke_result_t<decltype(Fn), Class&>>) {
        Fn(obj);
      } else {
        setReturn(vm, Fn(obj));
      }
    } else {
      [&obj, vm]<std::size_t... Is>(std::index_sequence<Is...>) {
        if constexpr (std::is_void_v<std::invoke_result_t<decltype(Fn), Class&, Args...>>) {
          Fn(obj, getArg<Args>(vm, static_cast<int>(Is) + 1)...);
        } else {
          setReturn(vm, Fn(obj, getArg<Args>(vm, static_cast<int>(Is) + 1)...));
        }
      }(std::index_sequence_for<Args...>{});
    }
  }

  // --- External property getter trampoline (propExt read-only) ---
  // Calls a free function taking the foreign object, returning its result.
  template <auto Getter, typename Class>
  void propExtGetterTrampoline(WrenVM* vm) {
    Class& obj = getForeign<Class>(vm, 0);
    setReturn(vm, Getter(obj));
  }

  // --- External property setter trampoline (propExt write) ---
  // Calls a free function taking the foreign object and the new value.
  template <auto Setter, typename Class, typename ValType>
  void propExtSetterTrampoline(WrenVM* vm) {
    Class& obj = getForeign<Class>(vm, 0);
    Setter(obj, getArg<ValType>(vm, 1));
  }

  // --- Constructor (allocator) trampoline ---
// Creates a shared_ptr<T> and stores it in a ForeignWrapper in the foreign slot.
template <typename T, typename... Args>
void ctorTrampoline(WrenVM* vm) {
  wrenEnsureSlots(vm, static_cast<int>(sizeof...(Args)) + 1);
  void* data = wrenSetSlotNewForeign(vm, 0, 0, sizeof(ForeignWrapper));
  if constexpr (sizeof...(Args) == 0) {
    new (data) ForeignWrapper(ForeignWrapper::make<T>());
  } else {
    [data, vm]<std::size_t... Is>(std::index_sequence<Is...>) {
      new (data) ForeignWrapper(ForeignWrapper::make<T>(
        getArg<Args>(vm, static_cast<int>(Is) + 1)...));
    }(std::index_sequence_for<Args...>{});
  }
}

// --- Finalizer (destructor) trampoline ---
// All foreign objects use the same finalizer: destruct the ForeignWrapper,
// which resets the shared_ptr (and destructs the C++ object if refcount hits 0).
template <typename T>
void finalizeTrampoline(void* data) {
  std::launder(reinterpret_cast<ForeignWrapper*>(data))->~ForeignWrapper();
}

// --- Constructor registry: maps arg count -> constructor trampoline, per type ---
template <typename T>
struct CtorRegistry {
  static std::unordered_map<int, WrenForeignMethodFn> ctors;
};
template <typename T>
std::unordered_map<int, WrenForeignMethodFn> CtorRegistry<T>::ctors;

// --- Constructor dispatcher: selects the right constructor based on slot count ---
template <typename T>
void ctorDispatcher(WrenVM* vm) {
  int numArgs = wrenGetSlotCount(vm) - 1; // slot 0 is receiver
  auto it = CtorRegistry<T>::ctors.find(numArgs);
  if (it != CtorRegistry<T>::ctors.end()) {
    it->second(vm);
  }
}

// ===========================================================================
// Signature generation helpers
// ===========================================================================
inline std::string makeMethodSig(std::string_view name, std::size_t numArgs, bool isStatic) {
  // NOTE: Wren's bindForeignMethodFn callback signature does NOT include "static ".
  // The isStatic parameter tells us, and the signature is just the method name + args.
  std::string sig = std::string(name);
  if (numArgs == 0) return sig;
  sig += "(";
  for (std::size_t i = 0; i < numArgs; ++i) {
    if (i > 0) sig += ",";
    sig += "_";
  }
  sig += ")";
  return sig;
}

inline std::string makeCtorSig(std::string_view className, std::size_t numArgs) {
  // For constructor source gen, we use arg0, arg1, etc.
  // But for the ctorSig stored (used to generate Wren source), we just store
  // the class name — the source generation in Module::addClass handles the rest.
  return std::string(className) + "(" + std::to_string(numArgs) + ")";
}

inline std::string makeSetterSig(std::string_view name) {
  return std::string(name) + "=(_)";
}

} // namespace detail

// ===========================================================================
// Handle — RAII wrapper for WrenHandle
// ===========================================================================
class Handle {
public:
  Handle() = default;
  Handle(WrenVM* vm, WrenHandle* h) : vm_(vm), h_(h) {}
  ~Handle() { release(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& o) noexcept : vm_(o.vm_), h_(o.h_) { o.vm_ = nullptr; o.h_ = nullptr; }
  Handle& operator=(Handle&& o) noexcept {
    if (this != &o) { release(); vm_ = o.vm_; h_ = o.h_; o.vm_ = nullptr; o.h_ = nullptr; }
    return *this;
  }

  WrenHandle* raw() const { return h_; }
  explicit operator bool() const { return h_ != nullptr; }

  template <typename... Args>
  WrenInterpretResult operator()(Args&&... args) {
    if (!vm_ || !h_) return WREN_RESULT_RUNTIME_ERROR;
    wrenEnsureSlots(vm_, sizeof...(Args) + 1);
    int s = 1;
    (detail::Converter<std::decay_t<Args>>::set(vm_, s++, std::forward<Args>(args)), ...);
    return wrenCall(vm_, h_);
  }

private:
  void release() { if (vm_ && h_) wrenReleaseHandle(vm_, h_); }
  WrenVM* vm_ = nullptr;
  WrenHandle* h_ = nullptr;
};

// ===========================================================================
// Foreign<T> — fluent builder for registering a foreign class
//   Returned by value from Module::klass<T>(). Auto-registers on destruction.
// ===========================================================================
template <typename T>
class Foreign {
public:
  Foreign(Module* mod, std::string className) : module_(mod) {
    reg_.className = std::move(className);
  }
  ~Foreign();
  Foreign(const Foreign&) = delete;
  Foreign& operator=(const Foreign&) = delete;
  Foreign(Foreign&&) = default;
  Foreign& operator=(Foreign&&) = delete;

  template <typename... Args>
  Foreign<T>& ctor() {
    detail::CtorRegistry<T>::ctors[static_cast<int>(sizeof...(Args))] = &detail::ctorTrampoline<T, Args...>;
    reg_.allocator = &detail::ctorDispatcher<T>;
    reg_.ctorArgCounts.push_back(static_cast<int>(sizeof...(Args)));
    // With the shared_ptr wrapper, we always need a finalizer to destruct
    // the ForeignWrapper. Auto-register it here.
    reg_.finalizer = &detail::finalizeTrampoline<T>;
    return *this;
  }

  template <auto Fn, typename... Args>
  Foreign<T>& func(std::string_view name) {
    reg_.methods.push_back({
      &detail::memberTrampoline<Fn, T, Args...>,
      false,
      detail::makeMethodSig(name, sizeof...(Args), false)
    });
    return *this;
  }

  template <auto Fn, typename... Args>
  Foreign<T>& funcStatic(std::string_view name) {
    reg_.methods.push_back({
      &detail::freeTrampoline<Fn, Args...>,
      true,
      detail::makeMethodSig(name, sizeof...(Args), true)
    });
    return *this;
  }

  template <auto Getter>
  Foreign<T>& propReadonly(std::string_view name) {
    reg_.methods.push_back({
      &detail::getterTrampoline<Getter, T>,
      false,
      detail::makeMethodSig(name, 0, false)
    });
    return *this;
  }

  // Register a member function getter/setter pair as a read-write property.
  // The value type is deduced from the setter's parameter.
  template <auto Getter, auto Setter>
  Foreign<T>& prop(std::string_view name) {
    using ValType = detail::setter_value_t<decltype(Setter)>;
    propReadonly<Getter>(name);
    reg_.methods.push_back({
      &detail::setterTrampoline<Setter, T, ValType>,
      false,
      detail::makeSetterSig(name)
    });
    return *this;
  }

  // Register a direct member variable as a read-only Wren property.
  // Uses a member object pointer (e.g., &Point::x) to generate a getter.
  template <auto MemberPtr>
  Foreign<T>& varReadOnly(std::string_view name) {
    reg_.methods.push_back({
      &detail::getterTrampoline<MemberPtr, T>,
      false,
      detail::makeMethodSig(name, 0, false)
    });
    return *this;
  }

  // Register a direct member variable as a read-write Wren property.
  // Uses a member object pointer (e.g., &Point::x) to generate both
  // a getter and a setter. The member type is auto-deduced.
  template <auto MemberPtr>
  Foreign<T>& var(std::string_view name) {
    using MemberType = std::decay_t<decltype(std::declval<T>().*MemberPtr)>;
    varReadOnly<MemberPtr>(name);
    reg_.methods.push_back({
      &detail::setterTrampoline<MemberPtr, T, MemberType>,
      false,
      detail::makeSetterSig(name)
    });
    return *this;
  }

  // Register an external (free) function as a method on this class.
  // The function receives the foreign object as its first argument.
  // This mirrors wrenbind17's funcExt: `funcExt<&freeFunction, ArgTypes...>("name")`
  template <auto Fn, typename... Args>
  Foreign<T>& funcExt(std::string_view name) {
    reg_.methods.push_back({
      &detail::funcExtTrampoline<Fn, T, Args...>,
      false,
      detail::makeMethodSig(name, sizeof...(Args), false)
    });
    return *this;
  }

  // Register an external (free) function as a read-only property getter.
  // The function receives the foreign object and returns the property value.
  // This mirrors wrenbind17's propExt for read-only access.
  template <auto Getter>
  Foreign<T>& propExtReadonly(std::string_view name) {
    reg_.methods.push_back({
      &detail::propExtGetterTrampoline<Getter, T>,
      false,
      detail::makeMethodSig(name, 0, false)
    });
    return *this;
  }

  // Register external (free) functions as a read-write property.
  // The getter receives the foreign object and returns the value.
  // The setter receives the foreign object and the new value.
  // This mirrors wrenbind17's propExt for read-write access.
  // The value type is deduced from the setter's parameter.
  template <auto Getter, auto Setter>
  Foreign<T>& propExt(std::string_view name) {
    using ValType = detail::setter_value_t<decltype(Setter)>;
    propExtReadonly<Getter>(name);
    reg_.methods.push_back({
      &detail::propExtSetterTrampoline<Setter, T, ValType>,
      false,
      detail::makeSetterSig(name)
    });
    return *this;
  }

  // With shared_ptr wrappers, finalize() is automatically handled by ctor().
  // This method is kept for API compatibility but is now a no-op.
  Foreign<T>& finalize() {
    // Already set in ctor() — nothing additional needed.
    return *this;
  }

private:
  Module* module_;
  ClassReg reg_;
  friend class Module;
};

// ===========================================================================
// Module — builder for registering a Wren module with foreign classes
// ===========================================================================
class Module {
public:
  Module(VM* vm, std::string name) : vm_(vm) { reg_.name = std::move(name); }

  template <typename T>
  Foreign<T> klass(std::string className) {
    return Foreign<T>(this, std::move(className));
  }

  Module& source(std::string wrenSource) {
    reg_.extraSource += wrenSource;
    return *this;
  }

  void addClass(ClassReg reg) {
    std::string src = "foreign class " + reg.className + " {\n";
    // Generate all registered constructor overloads.
    for (int numArgs : reg.ctorArgCounts) {
      std::string ctorSrc = "  construct new(";
      for (int i = 0; i < numArgs; ++i) {
        if (i > 0) ctorSrc += ",";
        ctorSrc += "arg" + std::to_string(i);
      }
      ctorSrc += ") {}\n";
      src += ctorSrc;
    }
    // Track which (isStatic, signature) pairs have already been emitted to
    // avoid generating duplicate foreign method declarations in Wren source.
    std::vector<std::pair<bool, std::string>> emitted;
    for (const auto& m : reg.methods) {
      auto key = std::make_pair(m.isStatic, m.signature);
      bool alreadyEmitted = false;
      for (const auto& e : emitted) {
        if (e == key) { alreadyEmitted = true; break; }
      }
      if (alreadyEmitted) continue;
      emitted.push_back(key);

      // Add "static " prefix for static methods in Wren source
      std::string prefix = m.isStatic ? "static " : "";
      // Convert bind signature (e.g. "dot(_,_)") to Wren source (e.g. "dot(arg0,arg1)")
      std::string wrenSig = m.signature;
      bool isZeroArg = (wrenSig.find('(') == std::string::npos);
      std::string::size_type pos = 0;
      int argIdx = 0;
      while ((pos = wrenSig.find('_', pos)) != std::string::npos) {
        std::string argName = "arg" + std::to_string(argIdx++);
        wrenSig.replace(pos, 1, argName);
        pos += argName.size();
      }
      src += "  foreign " + prefix + wrenSig + "\n";
      // For 0-arg methods, also generate the "()" call form so both
      // `obj.foo` (getter) and `obj.foo()` (method call) work.
      if (isZeroArg) {
        src += "  foreign " + prefix + wrenSig + "()\n";
      }
    }
    src += "}\n";
    reg.wrenSource = src;
    reg_.classes.push_back(std::move(reg));
  }

  const std::string& name() const { return reg_.name; }
  VM* vm() const { return vm_; }
  ModuleReg& registration() { return reg_; }

  std::string generateSource() const {
    std::string src;
    for (const auto& cls : reg_.classes) { src += cls.wrenSource + "\n"; }
    src += reg_.extraSource;
    return src;
  }

private:
  VM* vm_;
  ModuleReg reg_;
  friend class VM;
  template <typename> friend class Foreign;
};

template <typename T>
Foreign<T>::~Foreign() {
  module_->addClass(std::move(reg_));
}

// ===========================================================================
// VM — the main C++ entry point
// ===========================================================================
class VM {
public:
  struct Config {
    WrenConfiguration wrenConfig;
    Config() { wrenInitConfiguration(&wrenConfig); }
  };

  VM() : VM(Config{}) {}
  VM(Config config) : config_(std::move(config)) {
    config_.wrenConfig.userData = this;
    config_.wrenConfig.bindForeignMethodFn = &VM::bindMethodCb;
    config_.wrenConfig.bindForeignClassFn = &VM::bindClassCb;
    // Auto-serve source for modules registered via .module()
    if (!config_.wrenConfig.loadModuleFn) {
      config_.wrenConfig.loadModuleFn = &VM::loadModuleCb;
    }
    vm_ = wrenNewVM(&config_.wrenConfig);
  }
  ~VM() { if (vm_) wrenFreeVM(vm_); }
  VM(const VM&) = delete;
  VM& operator=(const VM&) = delete;
  VM(VM&&) = delete;
  VM& operator=(VM&&) = delete;

  WrenVM* raw() const { return vm_; }

  Module& module(std::string name) {
    auto& m = modules_[name];
    if (!m) m = std::make_unique<Module>(this, name);
    return *m;
  }

  WrenInterpretResult interpret(std::string_view mod, std::string_view src) {
    return wrenInterpret(vm_, std::string(mod).c_str(), std::string(src).c_str());
  }

  Handle callHandle(std::string_view sig) {
    return Handle(vm_, wrenMakeCallHandle(vm_, std::string(sig).c_str()));
  }

  void getVariable(std::string_view mod, std::string_view name, int slot) {
    wrenEnsureSlots(vm_, slot + 1);
    wrenGetVariable(vm_, std::string(mod).c_str(), std::string(name).c_str(), slot);
  }

  void collectGarbage() { wrenCollectGarbage(vm_); }
  void* userData() const { return wrenGetUserData(vm_); }
  void setUserData(void* data) { wrenSetUserData(vm_, data); }

  // Get a foreign object from a slot as a typed reference.
  template <typename T> T get(int slot) {
    using D = std::decay_t<T>;
    if constexpr (detail::isWrenBuiltin_v<D>) return detail::Converter<D>::get(vm_, slot);
    else return detail::getForeign<D>(vm_, slot);
  }

  // Get a shared_ptr to a foreign object — enables C++ to keep the object alive.
  template <typename T>
  std::shared_ptr<T> getShared(int slot) {
    return detail::getForeignShared<T>(vm_, slot);
  }

  // Set a slot to a foreign object (copies/moves the value into a new wrapper).
  template <typename T> void set(int slot, T&& value) {
    using D = std::decay_t<T>;
    if constexpr (detail::isWrenBuiltin_v<D>) detail::Converter<D>::set(vm_, slot, std::forward<T>(value));
    else {
      void* data = wrenSetSlotNewForeign(vm_, slot, slot, sizeof(detail::ForeignWrapper));
      new (data) detail::ForeignWrapper(detail::ForeignWrapper::make<D>(std::forward<T>(value)));
    }
  }

  // Set a slot from an existing shared_ptr — shares ownership, no copy.
  template <typename T>
  void setShared(int slot, std::shared_ptr<T> ptr) {
    void* data = wrenSetSlotNewForeign(vm_, slot, slot, sizeof(detail::ForeignWrapper));
    new (data) detail::ForeignWrapper(detail::ForeignWrapper::share<T>(std::move(ptr)));
  }

  void setNull(int slot) { wrenSetSlotNull(vm_, slot); }
  int slotCount() const { return wrenGetSlotCount(vm_); }
  void ensureSlots(int n) { wrenEnsureSlots(vm_, n); }
  WrenType slotType(int s) const { return wrenGetSlotType(vm_, s); }

private:
  static WrenForeignMethodFn bindMethodCb(WrenVM* vm, const char* mod,
                                          const char* cls, bool isStatic,
                                          const char* sig) {
    auto* self = static_cast<VM*>(wrenGetUserData(vm));
    auto it = self->modules_.find(mod);
    if (it == self->modules_.end()) return nullptr;
    for (const auto& c : it->second->registration().classes) {
      if (c.className != cls) continue;
      // Exact match first
      for (const auto& m : c.methods) {
        if (m.isStatic == isStatic && m.signature == sig) return m.fn;
      }
      // Fallback: Wren distinguishes getter "foo" from call "foo()" for 0-arg methods.
      // Allow either form to match a 0-arg method registration.
      std::string reqSig(sig);
      std::string altSig;
      if (reqSig.size() > 2 && reqSig.compare(reqSig.size() - 2, 2, "()") == 0) {
        altSig = reqSig.substr(0, reqSig.size() - 2); // "foo()" -> "foo"
      } else if (reqSig.find('(') == std::string::npos) {
        altSig = reqSig + "()"; // "foo" -> "foo()"
      }
      if (!altSig.empty()) {
        for (const auto& m : c.methods) {
          if (m.isStatic == isStatic && m.signature == altSig) return m.fn;
        }
      }
    }
    return nullptr;
  }

  static WrenLoadModuleResult loadModuleCb(WrenVM* vm, const char* mod) {
    auto* self = static_cast<VM*>(wrenGetUserData(vm));
    WrenLoadModuleResult result{};
    auto it = self->modules_.find(mod);
    if (it != self->modules_.end()) {
      // The module has registered foreign classes — serve its generated source.
      // We need to keep the string alive; store it in a static to survive the callback.
      // (The VM copies the source during compilation, so it's safe after that.)
      static thread_local std::string src;
      src = it->second->generateSource();
      if (!src.empty()) {
        result.source = src.c_str();
      }
    }
    return result;
  }

  static WrenForeignClassMethods bindClassCb(WrenVM* vm, const char* mod, const char* cls) {
    auto* self = static_cast<VM*>(wrenGetUserData(vm));
    WrenForeignClassMethods m{nullptr, nullptr};
    auto it = self->modules_.find(mod);
    if (it == self->modules_.end()) return m;
    for (const auto& c : it->second->registration().classes) {
      if (c.className != cls) continue;
      m.allocate = c.allocator;
      m.finalize = c.finalizer;
      break;
    }
    return m;
  }

  Config config_;
  WrenVM* vm_ = nullptr;
  std::unordered_map<std::string, std::unique_ptr<Module>> modules_;
};

} // namespace wren

#endif // wren_hpp