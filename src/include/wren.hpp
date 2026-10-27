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
// Key design: foreign objects live in the VM's ObjForeign as a shared_ptr
// payload (with an optional type tag) — see ObjForeign in wren_value.h. The
// VM owns a reference and releases the payload on collection, so:
//   - Safe shared ownership across the C++↔Wren boundary
//   - Automatic destruction without per-class finalizer trampolines
//   - Passing registered foreign types as method parameters
//
// Template-generated static trampoline functions serve as the C function pointers
// that Wren's VM expects. Each unique <method-pointer, Class, Args...> instantiation
// generates a unique function address, enabling zero-overhead dispatch.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

extern "C" {
  #include "wren.h"
}
#include "wren_debugger.h"

// The single VM class: WrenVM itself, which now carries the C++ host API and
// the virtual host callbacks. wren::VM is an alias so existing code keeps
// compiling.
#include "../vm/wren_vm.h"

namespace wren {

using VM = ::WrenVM;
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
  // The class is declared manually via Module::source() instead of a
  // generated bare "foreign class" declaration. Bindings still dispatch by
  // class name.
  bool manualSource = false;
};

struct ModuleReg {
  std::string name;
  std::vector<ClassReg> classes;
  std::string extraSource;
};

// ===========================================================================
// Any — an owning handle to an arbitrary Wren value, captured from a slot.
// Lets a foreign method or constructor receive values the type system can't
// describe (lists, maps, fibers, ...): the value is pinned with
// wrenGetSlotHandle and released when the Any is destroyed.
//
// Lifetime note: the captured handle is released against the VM the value
// was captured from, so an Any (or its owner) must not outlive that VM
// unless the host releases it first (reset()) — e.g. via a registry that
// runs before VM teardown.
// ===========================================================================
class Any {
public:
  Any() = default;

  // Captures the value in [slot].
  Any(WrenVM* vm, int slot) : vm_(vm), h_(wrenGetSlotHandle(vm, slot)) {}

  // Adopts an existing handle; it is released when the Any is destroyed.
  Any(WrenVM* vm, WrenHandle* handle) : vm_(vm), h_(handle) {}

  ~Any() { reset(); }

  Any(const Any&) = delete;
  Any& operator=(const Any&) = delete;
  Any(Any&& o) noexcept : vm_(o.vm_), h_(o.h_) { o.vm_ = nullptr; o.h_ = nullptr; }
  Any& operator=(Any&& o) noexcept {
    if (this != &o) {
      reset();
      vm_ = o.vm_;
      h_ = o.h_;
      o.vm_ = nullptr;
      o.h_ = nullptr;
    }
    return *this;
  }

  // Releases the captured value (against the VM it was captured from) and
  // empties the Any. Safe on an empty Any.
  void reset() {
    if (vm_ != nullptr && h_ != nullptr) wrenReleaseHandle(vm_, h_);
    vm_ = nullptr;
    h_ = nullptr;
  }

  WrenHandle* rawHandle() const { return h_; }
  WrenVM* vm() const { return vm_; }
  explicit operator bool() const { return h_ != nullptr; }

private:
  WrenVM* vm_ = nullptr;
  WrenHandle* h_ = nullptr;
};

namespace detail {

// ===========================================================================
// ClassId — per-type record of the module/class name a C++ type is bound to
// (filled in by Foreign<T> when Module::klass<T>("Name") registers the
// class). setReturn uses it to look up the Wren class when boxing return
// values: during a foreign method call slot 0 holds the receiver, so the
// class has to be fetched into a scratch slot first.
// ===========================================================================
template <typename T>
struct ClassId {
  static inline std::string moduleName;
  static inline std::string className;
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

// Captures the arbitrary Wren value in [slot] (see class Any above).
template <> struct Converter<Any> {
  static Any get(WrenVM* vm, int slot) { return Any(vm, slot); }
  static void set(WrenVM* vm, int slot, const Any& v) {
    if (v.rawHandle() != nullptr) wrenSetSlotHandle(vm, slot, v.rawHandle());
    else wrenSetSlotNull(vm, slot);
  }
};

// Helper concept for built-in convertible types
template <typename T>
inline constexpr bool isWrenBuiltin_v =
  std::is_arithmetic_v<T> ||
  std::is_same_v<T, std::string> ||
  std::is_same_v<T, std::string_view> ||
  std::is_same_v<T, const char*>;

// Helper for shared_ptr detection: shared_ptr returns hand out the pointee
// with shared ownership instead of copying it.
template <typename T> struct is_shared_ptr : std::false_type {};
template <typename T> struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};
template <typename T>
inline constexpr bool is_shared_ptr_v = is_shared_ptr<std::decay_t<T>>::value;

// Helper for std::variant detection.
template <typename T> struct is_variant : std::false_type {};
template <typename... Ts> struct is_variant<std::variant<Ts...>> : std::true_type {};
template <typename T>
inline constexpr bool is_variant_v = is_variant<std::decay_t<T>>::value;

// Slot-value types: RAII wrappers around a Wren value/handle that convert
// through their Converter like builtins (Any for arbitrary values, Handle
// for method handles). Handle is defined below; getArg/setReturn only
// reference it through Converter<D>, so incompleteness is fine here.
template <typename T>
inline constexpr bool is_slot_value_v =
    std::is_same_v<T, Any> || std::is_same_v<T, Handle>;

// ===========================================================================
// Slot access helpers for foreign (wrapper-based) objects
// ===========================================================================

// Extract a typed reference from a foreign slot. The payload's shared_ptr
// lives in the VM's ObjForeign; the pointee is returned directly.
template <typename T>
inline T& getForeign(WrenVM* vm, int slot) {
  void* data = vm->GetSlotForeign(slot);
  return *static_cast<T*>(data);
}

// Extract a shared_ptr from a foreign slot — for shared ownership scenarios.
// Shares the VM's control block, so the pointee stays alive as long as the
// returned shared_ptr does, even if the Wren object is collected.
template <typename T>
inline std::shared_ptr<T> getForeignShared(WrenVM* vm, int slot) {
  return std::static_pointer_cast<T>(vm->GetSlotForeignShared(slot));
}

// Generic argument getter — handles builtins, variants, slot values,
// shared_ptr, foreign by-value/ref/ptr.
template <typename T>
inline decltype(auto) getArg(WrenVM* vm, int slot) {
  using D = std::decay_t<T>;
  if constexpr (isWrenBuiltin_v<D> || is_variant_v<D> || is_slot_value_v<D>) {
    return Converter<D>::get(vm, slot);
  } else if constexpr (is_shared_ptr_v<D>) {
    // shared_ptr argument: share the payload's ownership. The cast is
    // unchecked (static_pointer_cast off the shared_ptr<void> payload) —
    // polymorphic passes rely on the payload being the exact type or a
    // single-inheritance layout.
    return getForeignShared<typename D::element_type>(vm, slot);
  } else if constexpr (std::is_pointer_v<D>) {
    // Pointer argument: return address of the foreign object
    using PointedTo = std::remove_pointer_t<D>;
    return &getForeign<PointedTo>(vm, slot);
  } else {
    // Reference or value: return reference, caller copies if needed
    return getForeign<D>(vm, slot);
  }
}

// A scratch slot guaranteed to be clear of the current call's arguments.
inline int scratchSlot(WrenVM* vm) {
  const int slots = wrenGetSlotCount(vm);
  return slots < 8 ? 8 : slots;
}

// Fetch the Wren class registered for T (via Module::klass<T>) into a scratch
// slot and store [object] as a new foreign instance in [slot], recording
// T's static type on the instance.
template <typename T>
inline void setNewForeignInSlot(WrenVM* vm, int slot, std::shared_ptr<void> object) {
  const int scratch = scratchSlot(vm);
  const int classSlot = scratch > slot + 1 ? scratch : slot + 1;
  wrenEnsureSlots(vm, classSlot + 1);
  wrenGetVariable(vm, ClassId<T>::moduleName.c_str(), ClassId<T>::className.c_str(), classSlot);
  vm->SetSlotNewForeignShared(slot, classSlot, std::move(object), &typeid(T));
}

// setNewForeignInSlot into slot 0 (the common return-value case).
template <typename T>
inline void setNewForeignInSlot0(WrenVM* vm, std::shared_ptr<void> object) {
  setNewForeignInSlot<T>(vm, 0, std::move(object));
}

// The payload's recorded static type (the tag set when the instance was
// created through SetSlotNewForeignShared), or null when it carries none.
inline const std::type_info* slotForeignType(WrenVM* vm, int slot) {
  const Value value = vm->ApiStack()[slot];
  if (!IS_FOREIGN(value)) return nullptr;
  return AS_FOREIGN(value)->type;
}

// ===========================================================================
// Converter<std::variant<Ts...>> — maps a dynamically typed Wren slot onto
// a variant of the alternatives the host is prepared to receive.
//
// get() dispatches on the slot's Wren type:
//   null    -> the first nullptr_t / monostate alternative
//   bool    -> the bool alternative
//   num     -> the first integral alternative that holds the value
//              losslessly, else the first floating alternative
//   string  -> the first string-ish alternative
//   foreign -> the first alternative whose (element) type matches the
//              payload's recorded static type exactly
// set() visits the active alternative and pushes it back out: null-ish
// alternatives as null, arithmetic/string alternatives through their
// converter, shared_ptr<X> as a shared foreign instance, other class
// alternatives as a foreign copy.
// ===========================================================================
template <typename T> struct IsNullish : std::bool_constant<
    std::is_same_v<T, std::nullptr_t> || std::is_same_v<T, std::monostate>> {};
template <typename T> struct IsStringish : std::bool_constant<
    std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> ||
    std::is_same_v<T, const char*>> {};

template <typename... Ts>
struct Converter<std::variant<Ts...>> {
  using V = std::variant<Ts...>;

  // Index of the first alternative satisfying predicate template P, or -1.
  template <template <typename> class P, std::size_t I = 0>
  static constexpr int find() {
    if constexpr (I >= sizeof...(Ts)) return -1;
    else if constexpr (P<std::variant_alternative_t<I, V>>::value) return static_cast<int>(I);
    else return find<P, I + 1>();
  }

  static V get(WrenVM* vm, int slot) {
    switch (wrenGetSlotType(vm, slot)) {
      case WREN_TYPE_BOOL: {
        constexpr int B = find<IsBool>();
        if constexpr (B >= 0) return V(std::in_place_index<B>, wrenGetSlotBool(vm, slot));
        else return V{};
      }
      case WREN_TYPE_NUM:
        return getNum(vm, slot);
      case WREN_TYPE_STRING: {
        constexpr int S = find<IsStringish>();
        if constexpr (S >= 0) return V(std::in_place_index<S>, Converter<std::variant_alternative_t<S, V>>::get(vm, slot));
        else return V{};
      }
      case WREN_TYPE_NULL: {
        constexpr int N = find<IsNullish>();
        if constexpr (N >= 0) return V(std::in_place_index<N>);
        else return V{};
      }
      case WREN_TYPE_FOREIGN:
        return getForeignValue<0>(vm, slot);
      default:
        return V{};
    }
  }

  static void set(WrenVM* vm, int slot, const V& v) {
    std::visit([vm, slot](auto&& arg) {
      using A = std::decay_t<decltype(arg)>;
      if constexpr (IsNullish<A>::value) {
        wrenSetSlotNull(vm, slot);
      } else if constexpr (is_shared_ptr_v<A>) {
        setNewForeignInSlot<typename A::element_type>(vm, slot, std::shared_ptr<void>(arg));
      } else if constexpr (isWrenBuiltin_v<A>) {
        Converter<A>::set(vm, slot, arg);
      } else {
        setNewForeignInSlot<A>(vm, slot, std::make_shared<A>(arg));
      }
    }, v);
  }

private:
  template <typename T> struct IsBool : std::is_same<T, bool> {};

  template <std::size_t I = 0>
  static V getNum(WrenVM* vm, int slot) {
    const double d = wrenGetSlotDouble(vm, slot);
    return numAlt<I>(d);
  }

  // First integral alternative holding d losslessly, else first floating.
  template <std::size_t I = 0>
  static V numAlt(double d) {
    if constexpr (I >= sizeof...(Ts)) {
      constexpr int F = find<IsFloating>();
      if constexpr (F >= 0) {
        return V(std::in_place_index<F>,
                 static_cast<std::variant_alternative_t<F, V>>(d));
      } else {
        return V{};
      }
    } else {
      using A = std::variant_alternative_t<I, V>;
      if constexpr (std::is_integral_v<A> && !std::is_same_v<A, bool>) {
        if (std::floor(d) == d &&
            d >= static_cast<double>(std::numeric_limits<A>::min()) &&
            d <= static_cast<double>(std::numeric_limits<A>::max())) {
          return V(std::in_place_index<I>, static_cast<A>(d));
        }
      }
      return numAlt<I + 1>(d);
    }
  }

  template <typename T> struct IsFloating : std::is_floating_point<T> {};

  // Foreign slot: first alternative whose (element) type matches the
  // payload's recorded static type exactly.
  template <std::size_t I = 0>
  static V getForeignValue(WrenVM* vm, int slot) {
    const std::type_info* t = slotForeignType(vm, slot);
    if (t == nullptr) return V{};
    return foreignAlt<I>(*t, vm, slot);
  }

  template <std::size_t I = 0>
  static V foreignAlt(const std::type_info& t, WrenVM* vm, int slot) {
    if constexpr (I >= sizeof...(Ts)) {
      return V{};
    } else {
      using A = std::variant_alternative_t<I, V>;
      if constexpr (is_shared_ptr_v<A>) {
        if (t == typeid(typename A::element_type)) {
          return V(std::in_place_index<I>,
                   getForeignShared<typename A::element_type>(vm, slot));
        }
      } else if constexpr (std::is_class_v<A> && !IsStringish<A>::value) {
        if (t == typeid(A)) {
          return V(std::in_place_index<I>, getForeign<A>(vm, slot));
        }
      }
      return foreignAlt<I + 1>(t, vm, slot);
    }
  }
};

// Place a return value into slot 0. Builtins, variants and slot values use
// their converter; shared_ptr returns share ownership of the pointee; other
// types are foreign values, moved into a new VM-owned payload. The Wren
// class needed for boxing is looked up through the ClassId registry,
// because slot 0 holds the receiver during instance method calls.
template <typename T>
inline void setReturn(WrenVM* vm, T&& value) {
  using D = std::decay_t<T>;
  if constexpr (isWrenBuiltin_v<D> || is_variant_v<D> || is_slot_value_v<D>) {
    Converter<D>::set(vm, 0, std::forward<T>(value));
  } else if constexpr (is_shared_ptr_v<D>) {
    using X = typename D::element_type;
    setNewForeignInSlot0<X>(vm, std::shared_ptr<void>(std::forward<T>(value)));
  } else {
    setNewForeignInSlot0<D>(vm, std::make_shared<D>(std::forward<T>(value)));
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
      if constexpr (std::is_void_v<std::invoke_result_t<decltype(Fn), Class&, decltype(getArg<Args>(vm, 0))...>>) {
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
      if constexpr (std::is_void_v<std::invoke_result_t<decltype(Fn), decltype(getArg<Args>(vm, 0))...>>) {
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
        if constexpr (std::is_void_v<std::invoke_result_t<decltype(Fn), Class&, decltype(getArg<Args>(vm, 0))...>>) {
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
// Creates the C++ object and hands it to the VM, which stores it as the
// foreign instance's payload with shared ownership.
template <typename T, typename... Args>
void ctorTrampoline(WrenVM* vm) {
  wrenEnsureSlots(vm, static_cast<int>(sizeof...(Args)) + 1);
  if constexpr (sizeof...(Args) == 0) {
    vm->SetSlotNewForeignShared(0, 0, std::make_shared<T>(), &typeid(T));
  } else {
    [vm]<std::size_t... Is>(std::index_sequence<Is...>) {
      vm->SetSlotNewForeignShared(0, 0,
        std::make_shared<T>(getArg<Args>(vm, static_cast<int>(Is) + 1)...),
        &typeid(T));
    }(std::index_sequence_for<Args...>{});
  }
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
  // NOTE: The VM's bindForeignMethod() override receives the method signature
  // WITHOUT a "static " prefix. The isStatic parameter tells us, and the
  // signature is just the method name + args.
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

// A Handle argument captures the method receiver/value in the slot as a
// call handle; a Handle return stores it back. Used for passing callable
// values (e.g. fibers) through the boundary.
template <> struct detail::Converter<Handle> {
  static Handle get(WrenVM* vm, int slot) { return Handle(vm, wrenGetSlotHandle(vm, slot)); }
  static void set(WrenVM* vm, int slot, const Handle& h) {
    if (h.raw() != nullptr) wrenSetSlotHandle(vm, slot, h.raw());
    else wrenSetSlotNull(vm, slot);
  }
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
    // Destruction is automatic: the VM releases the shared_ptr payload when
    // the instance is collected, so no finalizer is needed.
    return *this;
  }

  // Declare this class manually via Module::source() (or in the importing
  // script) instead of letting addClass() generate a bare "foreign class"
  // declaration. Bindings still dispatch by class name.
  Foreign<T>& noSource() {
    reg_.manualSource = true;
    return *this;
  }

  // Install a raw allocate function instead of the generated constructor
  // trampolines — for allocators that can't be expressed as a C++
  // constructor call, e.g. one that aborts the fiber, or one that reads
  // raw slot values (handles, variants). [ctorArgCount], when >= 0, also
  // declares a constructor of that arity in the class source; the raw
  // allocator reads the arguments from the slots itself.
  Foreign<T>& allocatorRaw(WrenForeignMethodFn allocate, int ctorArgCount = -1) {
    reg_.allocator = allocate;
    if (ctorArgCount >= 0) {
      reg_.ctorArgCounts.push_back(ctorArgCount);
    }
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

  // Register a hand-written trampoline under [signature] (bind form, e.g.
  // "foo(_,_)", "trigger()" or "[_]"). For methods whose slot handling the
  // template builders can't express: variant arguments, shared_ptr
  // parameters, Wren values captured with wrenGetSlotHandle, and so on.
  Foreign<T>& funcRaw(std::string_view signature, WrenForeignMethodFn fn,
                      bool isStatic = false) {
    reg_.methods.push_back({fn, isStatic, std::string(signature)});
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

  // Destruction is automatic: the VM releases the payload's shared_ptr when
  // the instance is collected. This method is kept for API compatibility and
  // is a no-op.
  Foreign<T>& finalize() {
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
    if (reg.manualSource) {
      // The class is declared by hand; only record it for binding dispatch.
      reg_.classes.push_back(std::move(reg));
      return;
    }
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
      // Only plain getter-style names can be declared twice (bare and with
      // "()"); a "()" form of a subscript ("[_]"), setter ("foo=(_)") or
      // operator signature is a compile error in Wren.
      bool isGetterStyle = !m.signature.empty() &&
          m.signature.find_first_not_of(
              "abcdefghijklmnopqrstuvwxyz"
              "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
              std::string::npos;
      std::string::size_type pos = 0;
      int argIdx = 0;
      while ((pos = wrenSig.find('_', pos)) != std::string::npos) {
        std::string argName = "arg" + std::to_string(argIdx++);
        wrenSig.replace(pos, 1, argName);
        pos += argName.size();
      }
      src += "  foreign " + prefix + wrenSig + "\n";
      // For getter-style methods, also generate the "()" call form so both
      // `obj.foo` (getter) and `obj.foo()` (method call) work.
      if (isGetterStyle) {
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
  friend class ::WrenVM;
  template <typename> friend class Foreign;
};

template <typename T>
Foreign<T>::~Foreign() {
  // Record the module/class name for this type so return-value boxing
  // (setReturn) can find the Wren class later. Skip moved-from objects,
  // whose reg_ has already been transferred elsewhere.
  if (!reg_.className.empty()) {
    detail::ClassId<T>::moduleName = module_->name();
    detail::ClassId<T>::className = reg_.className;
  }
  module_->addClass(std::move(reg_));
}

} // namespace wren

// ===========================================================================
// WrenVM member definitions that need the converters, Handle and Module
// (declared in wren_vm.h). Defined at global scope because WrenVM lives
// there.
// ===========================================================================
inline wren::Module& WrenVM::Module(std::string name) {
  auto& m = modules_[name];
  if (!m) m = std::make_unique<wren::Module>(this, std::move(name));
  return *m;
}

inline wren::Handle WrenVM::CallHandle(std::string_view sig) {
  return wren::Handle(this, wrenMakeCallHandle(this, std::string(sig).c_str()));
}

template <typename T> inline T WrenVM::Get(int slot) {
  using D = std::decay_t<T>;
  if constexpr (wren::detail::isWrenBuiltin_v<D>) return wren::detail::Converter<D>::get(this, slot);
  else return wren::detail::getForeign<D>(this, slot);
}

template <typename T>
inline std::shared_ptr<T> WrenVM::GetShared(int slot) {
  return wren::detail::getForeignShared<T>(this, slot);
}

template <typename T> inline void WrenVM::Set(int slot, T&& value) {
  using D = std::decay_t<T>;
  if constexpr (wren::detail::isWrenBuiltin_v<D>) wren::detail::Converter<D>::set(this, slot, std::forward<T>(value));
  else SetSlotNewForeignShared(slot, slot, std::make_shared<D>(std::forward<T>(value)), &typeid(D));
}

template <typename T>
inline void WrenVM::SetShared(int slot, std::shared_ptr<T> ptr) {
  SetSlotNewForeignShared(slot, slot, std::shared_ptr<void>(std::move(ptr)), &typeid(T));
}

#endif // wren_hpp