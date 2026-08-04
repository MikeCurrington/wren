#ifndef wren_hpp
#define wren_hpp

// Wren C++20 API — A modern, type-safe interface for the Wren scripting language.
//
// This header provides:
//   - Fluent module/class/method registration (wrenbind17/pybind11 inspired)
//   - Type-safe foreign object storage via templates
//   - RAII handles and VM lifecycle management
//   - Type-safe slot access with automatic conversions
//
// The underlying VM (NaN-boxing, GC, bytecode) is unchanged. This layer wraps
// the raw C API with modern C++20 ergonomics.
//
// Key design: Template-generated static trampoline functions serve as the
// C function pointers that Wren's VM expects (WrenForeignMethodFn, etc.).
// Each unique <method-pointer, Class, Args...> instantiation generates a
// unique function address, enabling zero-overhead dispatch.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
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
  std::string ctorSig;
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
// Type converters: C++ type <-> Wren slot
// ===========================================================================
template <typename T> struct Converter;

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
template <> struct Converter<int32_t> {
  static int32_t get(WrenVM* vm, int slot) { return static_cast<int32_t>(wrenGetSlotDouble(vm, slot)); }
  static void set(WrenVM* vm, int slot, int32_t v) { wrenSetSlotDouble(vm, slot, static_cast<double>(v)); }
};
template <> struct Converter<int64_t> {
  static int64_t get(WrenVM* vm, int slot) { return static_cast<int64_t>(wrenGetSlotDouble(vm, slot)); }
  static void set(WrenVM* vm, int slot, int64_t v) { wrenSetSlotDouble(vm, slot, static_cast<double>(v)); }
};
template <> struct Converter<uint32_t> {
  static uint32_t get(WrenVM* vm, int slot) { return static_cast<uint32_t>(wrenGetSlotDouble(vm, slot)); }
  static void set(WrenVM* vm, int slot, uint32_t v) { wrenSetSlotDouble(vm, slot, static_cast<double>(v)); }
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
  std::is_same_v<T, double> || std::is_same_v<T, float> ||
  std::is_same_v<T, bool> ||
  std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t> ||
  std::is_same_v<T, uint32_t> ||
  std::is_same_v<T, std::string> ||
  std::is_same_v<T, std::string_view> ||
  std::is_same_v<T, const char*>;

// ===========================================================================
// Slot access helpers
// ===========================================================================

template <typename T>
inline T& getForeign(WrenVM* vm, int slot) {
  void* data = wrenGetSlotForeign(vm, slot);
  return *std::launder(reinterpret_cast<T*>(data));
}

template <typename T>
inline T getArg(WrenVM* vm, int slot) {
  if constexpr (isWrenBuiltin_v<T>) {
    return Converter<T>::get(vm, slot);
  } else {
    return getForeign<T>(vm, slot);
  }
}

template <typename T>
inline void setReturn(WrenVM* vm, T&& value) {
  using D = std::decay_t<T>;
  if constexpr (isWrenBuiltin_v<D>) {
    Converter<D>::set(vm, 0, std::forward<T>(value));
  } else {
    void* data = wrenSetSlotNewForeign(vm, 0, 0, sizeof(D));
    new (data) D(std::forward<T>(value));
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
    [obj, vm]<std::size_t... Is>(std::index_sequence<Is...>) {
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

// --- Constructor (allocator) trampoline ---
template <typename T, typename... Args>
void ctorTrampoline(WrenVM* vm) {
  wrenEnsureSlots(vm, static_cast<int>(sizeof...(Args)) + 1);
  void* data = wrenSetSlotNewForeign(vm, 0, 0, sizeof(T));
  if constexpr (sizeof...(Args) == 0) {
    new (data) T();
  } else {
    [data, vm]<std::size_t... Is>(std::index_sequence<Is...>) {
      new (data) T(getArg<Args>(vm, static_cast<int>(Is) + 1)...);
    }(std::index_sequence_for<Args...>{});
  }
}

// --- Finalizer (destructor) trampoline ---
template <typename T>
void finalizeTrampoline(void* data) {
  std::launder(reinterpret_cast<T*>(data))->~T();
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
    reg_.allocator = &detail::ctorTrampoline<T, Args...>;
    reg_.ctorSig = detail::makeCtorSig(reg_.className, sizeof...(Args));
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

  template <auto Getter, auto Setter, typename ValType>
  Foreign<T>& prop(std::string_view name) {
    propReadonly<Getter>(name);
    reg_.methods.push_back({
      &detail::setterTrampoline<Setter, T, ValType>,
      false,
      detail::makeSetterSig(name)
    });
    return *this;
  }

  Foreign<T>& finalize() {
    reg_.finalizer = &detail::finalizeTrampoline<T>;
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
    if (reg.allocator) {
      // ctorSig is "ClassName(N)" where N is arg count.
      // Parse N and generate "construct new(arg0, arg1, ...) {}"
      auto parenPos = reg.ctorSig.find('(');
      int numArgs = 0;
      if (parenPos != std::string::npos) {
        numArgs = std::stoi(reg.ctorSig.substr(parenPos + 1));
      }
      std::string ctorSrc = "  construct new(";
      for (int i = 0; i < numArgs; ++i) {
        if (i > 0) ctorSrc += ",";
        ctorSrc += "arg" + std::to_string(i);
      }
      ctorSrc += ") {}\n";
      src += ctorSrc;
    }
    for (const auto& m : reg.methods) {
      // Add "static " prefix for static methods in Wren source
      std::string prefix = m.isStatic ? "static " : "";
      // Convert bind signature (e.g. "dot(_,_)") to Wren source (e.g. "dot(arg0,arg1)")
      std::string wrenSig = m.signature;
      std::string::size_type pos = 0;
      int argIdx = 0;
      while ((pos = wrenSig.find('_', pos)) != std::string::npos) {
        std::string argName = "arg" + std::to_string(argIdx++);
        wrenSig.replace(pos, 1, argName);
        pos += argName.size();
      }
      src += "  foreign " + prefix + wrenSig + "\n";
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

  template <typename T> T get(int slot) {
    if constexpr (detail::isWrenBuiltin_v<T>) return detail::Converter<T>::get(vm_, slot);
    else return detail::getForeign<T>(vm_, slot);
  }

  template <typename T> void set(int slot, T&& value) {
    using D = std::decay_t<T>;
    if constexpr (detail::isWrenBuiltin_v<D>) detail::Converter<D>::set(vm_, slot, std::forward<T>(value));
    else { void* d = wrenSetSlotNewForeign(vm_, slot, slot, sizeof(D)); new (d) D(std::forward<T>(value)); }
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
      for (const auto& m : c.methods) {
        if (m.isStatic == isStatic && m.signature == sig) return m.fn;
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