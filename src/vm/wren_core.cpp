#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "wren_common.h"
#include "wren_core.h"
#include "wren_math.h"
#include "wren_primitive.h"
#include "wren_value.h"

#include "wren_core.wren.inc"

DEF_PRIMITIVE(bool_not)
{
  RETURN_BOOL(!AS_BOOL(args[0]));
}

DEF_PRIMITIVE(bool_toString)
{
  if (AS_BOOL(args[0]))
  {
    RETURN_VAL(CONST_STRING(vm, "true"));
  }
  else
  {
    RETURN_VAL(CONST_STRING(vm, "false"));
  }
}

DEF_PRIMITIVE(class_name)
{
  RETURN_OBJ(AS_CLASS(args[0])->name);
}

DEF_PRIMITIVE(class_supertype)
{
  ObjClass* classObj = AS_CLASS(args[0]);

  // Object has no superclass.
  if (classObj->superclass == nullptr) RETURN_NULL;

  RETURN_OBJ(classObj->superclass);
}

DEF_PRIMITIVE(class_toString)
{
  RETURN_OBJ(AS_CLASS(args[0])->name);
}

DEF_PRIMITIVE(class_attributes)
{
  RETURN_VAL(AS_CLASS(args[0])->attributes);
}

DEF_PRIMITIVE(fiber_new)
{
  if (!validateFn(vm, args[1], "Argument")) return false;

  ObjClosure* closure = AS_CLOSURE(args[1]);
  if (closure->fn->arity > 1)
  {
    RETURN_ERROR("Function cannot take more than one parameter.");
  }
  
  RETURN_OBJ(wrenConstruct<ObjFiber>(vm, 0, closure));
}

DEF_PRIMITIVE(fiber_abort)
{
  vm->Fiber()->error = args[1];

  // If the error is explicitly null, it's not really an abort.
  return IS_NULL(args[1]);
}

// Transfer execution to [fiber] coming from the current fiber whose stack has
// [args].
//
// [isCall] is true if [fiber] is being called and not transferred.
//
// [hasValue] is true if a value in [args] is being passed to the new fiber.
// Otherwise, `null` is implicitly being passed.
bool WrenVM::RunFiber(ObjFiber* fiber, Value* args, bool isCall,
                     bool hasValue, const char* verb)
{
  // Alias so the shared RETURN_ERROR() macros -- written for the
  // fn(WrenVM*, Value*) primitive signature -- work in this member.
  WrenVM* vm = this;

  if (wrenHasError(fiber))
  {
    RETURN_ERROR_FMT("Cannot $ an aborted fiber.", verb);
  }

  // A fiber that is suspended in a foreign method waiting for a re-entrant
  // call to complete can't be resumed. Its stack is in use by the C code that
  // called the foreign method, so its state is only meaningful to that code.
  // Resuming it would unwind two interpreters at once, so prevent it.
  for (WrenCallContext* context = this->callContexts;
       context != nullptr;
       context = context->prev)
  {
    if (context->fiber == fiber)
    {
      RETURN_ERROR_FMT("Cannot $ a fiber suspended in a foreign call.", verb);
    }
  }

  if (isCall)
  {
    // You can't call a called fiber, but you can transfer directly to it,
    // which is why this check is gated on `isCall`. This way, after resuming a
    // suspended fiber, it will run and then return to the fiber that called it
    // and so on.
    if (fiber->caller != nullptr) RETURN_ERROR("Fiber has already been called.");

    if (fiber->state == FIBER_ROOT) RETURN_ERROR("Cannot call root fiber.");
    
    // Remember who ran it.
    fiber->caller = this->fiber;
  }

  if (fiber->numFrames == 0)
  {
    RETURN_ERROR_FMT("Cannot $ a finished fiber.", verb);
  }

  // When the calling fiber resumes, we'll store the result of the call in its
  // stack. If the call has two arguments (the fiber and the value), we only
  // need one slot for the result, so discard the other slot now.
  if (hasValue) this->fiber->stackTop--;

  if (fiber->numFrames == 1 &&
      fiber->frames[0].ip == fiber->frames[0].closure->fn->code.data())
  {
    // The fiber is being started for the first time. If its function takes a
    // parameter, bind an argument to it.
    if (fiber->frames[0].closure->fn->arity == 1)
    {
      fiber->stackTop[0] = hasValue ? args[1] : NULL_VAL;
      fiber->stackTop++;
    }
  }
  else
  {
    // The fiber is being resumed, make yield() or transfer() return the result.
    fiber->stackTop[-1] = hasValue ? args[1] : NULL_VAL;
  }

  this->fiber = fiber;
  return false;
}

DEF_PRIMITIVE(fiber_call)
{
  return vm->RunFiber(AS_FIBER(args[0]), args, true, false, "call");
}

DEF_PRIMITIVE(fiber_call1)
{
  return vm->RunFiber(AS_FIBER(args[0]), args, true, true, "call");
}

DEF_PRIMITIVE(fiber_current)
{
  RETURN_OBJ(vm->Fiber());
}

DEF_PRIMITIVE(fiber_error)
{
  RETURN_VAL(AS_FIBER(args[0])->error);
}

DEF_PRIMITIVE(fiber_exit)
{
  ObjFiber* current = vm->Fiber();

  // The fiber will never run again, so close every upvalue it still has open,
  // as if all of its functions had returned.
  wrenCloseUpvalues(current, current->stack);

  // Discard all of its call frames, marking the fiber as finished.
  current->numFrames = 0;

  // Unhook this fiber from the one that called it.
  ObjFiber* resumingFiber = current->caller;
  current->caller = nullptr;
  current->state = FIBER_OTHER;

  if (resumingFiber != nullptr)
  {
    // Make the caller's call() or try() return the exit value.
    resumingFiber->stackTop[-1] = args[1];
    vm->SetFiber(resumingFiber);
  }
  else
  {
    // There is no fiber to return to. Store the final result value at the
    // beginning of the stack so the C API can get it. Leaving [fiber]
    // pointing at this (now finished) fiber tells the interpreter to stop.
    current->stack[0] = args[1];
    current->stackTop = current->stack + 1;
  }

  return false;
}

DEF_PRIMITIVE(fiber_isDone)
{
  ObjFiber* runFiber = AS_FIBER(args[0]);
  RETURN_BOOL(runFiber->numFrames == 0 || wrenHasError(runFiber));
}

DEF_PRIMITIVE(fiber_suspend)
{
  // Switching to a null fiber tells the interpreter to stop and exit.
  vm->SetFiber(nullptr);
  vm->SetApiStack(nullptr);
  return false;
}

DEF_PRIMITIVE(fiber_transfer)
{
  return vm->RunFiber(AS_FIBER(args[0]), args, false, false, "transfer to");
}

DEF_PRIMITIVE(fiber_transfer1)
{
  return vm->RunFiber(AS_FIBER(args[0]), args, false, true, "transfer to");
}

DEF_PRIMITIVE(fiber_transferError)
{
  vm->RunFiber(AS_FIBER(args[0]), args, false, true, "transfer to");
  vm->Fiber()->error = args[1];
  return false;
}

DEF_PRIMITIVE(fiber_try)
{
  vm->RunFiber(AS_FIBER(args[0]), args, true, false, "try");
  
  // If we're switching to a valid fiber to try, remember that we're trying it.
  if (!wrenHasError(vm->Fiber())) vm->Fiber()->state = FIBER_TRY;
  return false;
}

DEF_PRIMITIVE(fiber_try1)
{
  vm->RunFiber(AS_FIBER(args[0]), args, true, true, "try");
  
  // If we're switching to a valid fiber to try, remember that we're trying it.
  if (!wrenHasError(vm->Fiber())) vm->Fiber()->state = FIBER_TRY;
  return false;
}

DEF_PRIMITIVE(fiber_yield)
{
  ObjFiber* current = vm->Fiber();
  vm->SetFiber(current->caller);

  // Unhook this fiber from the one that called it.
  current->caller = nullptr;
  current->state = FIBER_OTHER;

  if (vm->Fiber() != nullptr)
  {
    // Make the caller's run method return null.
    vm->Fiber()->stackTop[-1] = NULL_VAL;
  }

  return false;
}

DEF_PRIMITIVE(fiber_yield1)
{
  ObjFiber* current = vm->Fiber();
  vm->SetFiber(current->caller);

  // Unhook this fiber from the one that called it.
  current->caller = nullptr;
  current->state = FIBER_OTHER;

  if (vm->Fiber() != nullptr)
  {
    // Make the caller's run method return the argument passed to yield.
    vm->Fiber()->stackTop[-1] = args[1];

    // When the yielding fiber resumes, we'll store the result of the yield
    // call in its stack. Since Fiber.yield(value) has two arguments (the Fiber
    // class and the value) and we only need one slot for the result, discard
    // the other slot now.
    current->stackTop--;
  }

  return false;
}

DEF_PRIMITIVE(fn_new)
{
  if (!validateFn(vm, args[1], "Argument")) return false;

  // The block argument is already a function, so just return it.
  RETURN_VAL(args[1]);
}

DEF_PRIMITIVE(fn_arity)
{
  RETURN_NUM(AS_CLOSURE(args[0])->fn->arity);
}

void WrenVM::CallFn(Value* args, int numArgs)
{
  // +1 to include the function itself.
  this->CallFunction( this->fiber, AS_CLOSURE(args[0]), numArgs + 1);
}

#define DEF_FN_CALL(numArgs)                                                   \
    DEF_PRIMITIVE(fn_call##numArgs)                                            \
    {                                                                          \
      vm->CallFn(args, numArgs);                                              \
      return false;                                                            \
    }

DEF_FN_CALL(0)
DEF_FN_CALL(1)
DEF_FN_CALL(2)
DEF_FN_CALL(3)
DEF_FN_CALL(4)
DEF_FN_CALL(5)
DEF_FN_CALL(6)
DEF_FN_CALL(7)
DEF_FN_CALL(8)
DEF_FN_CALL(9)
DEF_FN_CALL(10)
DEF_FN_CALL(11)
DEF_FN_CALL(12)
DEF_FN_CALL(13)
DEF_FN_CALL(14)
DEF_FN_CALL(15)
DEF_FN_CALL(16)

DEF_PRIMITIVE(fn_toString)
{
  RETURN_VAL(CONST_STRING(vm, "<fn>"));
}

// Creates a new list of size args[1], with all elements initialized to args[2].
DEF_PRIMITIVE(list_filled)
{
  if (!validateInt(vm, args[1], "Size")) return false;  
  if (AS_NUM(args[1]) < 0) RETURN_ERROR("Size cannot be negative.");
  
  uint32_t size = (uint32_t)AS_NUM(args[1]);
  ObjList* list = wrenConstruct<ObjList>(vm, 0, size);
  
  for (uint32_t i = 0; i < size; i++)
  {
    list->elements[i] = args[2];
  }
  
  RETURN_OBJ(list);
}

DEF_PRIMITIVE(list_new)
{
  RETURN_OBJ(wrenConstruct<ObjList>(vm, 0, 0));
}

DEF_PRIMITIVE(list_add)
{
  AS_LIST(args[0])->elements.push_back(args[1]);
  RETURN_VAL(args[1]);
}

// Adds an element to the list and then returns the list itself. This is called
// by the compiler when compiling list literals instead of using add() to
// minimize stack churn.
DEF_PRIMITIVE(list_addCore)
{
  AS_LIST(args[0])->elements.push_back(args[1]);
  
  // Return the list.
  RETURN_VAL(args[0]);
}

DEF_PRIMITIVE(list_clear)
{
  AS_LIST(args[0])->elements.clear();
  RETURN_NULL;
}

DEF_PRIMITIVE(list_count)
{
  RETURN_NUM(AS_LIST(args[0])->elements.size());
}

DEF_PRIMITIVE(list_insert)
{
  ObjList* list = AS_LIST(args[0]);

  // count + 1 here so you can "insert" at the very end.
  uint32_t index = validateIndex(vm, args[1], list->elements.size() + 1,
                                 "Index");
  if (index == UINT32_MAX) return false;

  vm->ListInsert( list, args[2], index);
  RETURN_VAL(args[2]);
}

DEF_PRIMITIVE(list_iterate)
{
  ObjList* list = AS_LIST(args[0]);

  // If we're starting the iteration, return the first index.
  if (IS_NULL(args[1]))
  {
    if (list->elements.size() == 0) RETURN_FALSE;
    RETURN_NUM(0);
  }

  if (!validateInt(vm, args[1], "Iterator")) return false;

  // Stop if we're out of bounds.
  double index = AS_NUM(args[1]);
  if (index < 0 || index >= list->elements.size() - 1) RETURN_FALSE;

  // Otherwise, move to the next index.
  RETURN_NUM(index + 1);
}

DEF_PRIMITIVE(list_iteratorValue)
{
  ObjList* list = AS_LIST(args[0]);
  uint32_t index = validateIndex(vm, args[1], list->elements.size(), "Iterator");
  if (index == UINT32_MAX) return false;

  RETURN_VAL(list->elements[index]);
}

DEF_PRIMITIVE(list_removeAt)
{
  ObjList* list = AS_LIST(args[0]);
  uint32_t index = validateIndex(vm, args[1], list->elements.size(), "Index");
  if (index == UINT32_MAX) return false;

  RETURN_VAL(vm->ListRemoveAt( list, index));
}

DEF_PRIMITIVE(list_swap)
{
  ObjList* list = AS_LIST(args[0]);
  uint32_t indexA = validateIndex(vm, args[1], list->elements.size(), "Index 0");
  if (indexA == UINT32_MAX) return false;
  uint32_t indexB = validateIndex(vm, args[2], list->elements.size(), "Index 1");
  if (indexB == UINT32_MAX) return false;

  Value a = list->elements[indexA];
  list->elements[indexA] = list->elements[indexB];
  list->elements[indexB] = a;

  RETURN_NULL;
}

DEF_PRIMITIVE(list_subscript)
{
  ObjList* list = AS_LIST(args[0]);

  if (IS_NUM(args[1]))
  {
    uint32_t index = validateIndex(vm, args[1], list->elements.size(),
                                   "Subscript");
    if (index == UINT32_MAX) return false;

    RETURN_VAL(list->elements[index]);
  }

  if (!IS_RANGE(args[1]))
  {
    RETURN_ERROR("Subscript must be a number or a range.");
  }

  int step;
  uint32_t count = list->elements.size();
  uint32_t start = calculateRange(vm, AS_RANGE(args[1]), &count, &step);
  if (start == UINT32_MAX) return false;

  ObjList* result = wrenConstruct<ObjList>(vm, 0, count);
  for (uint32_t i = 0; i < count; i++)
  {
    result->elements[i] = list->elements[start + i * step];
  }

  RETURN_OBJ(result);
}

DEF_PRIMITIVE(list_subscriptSetter)
{
  ObjList* list = AS_LIST(args[0]);
  uint32_t index = validateIndex(vm, args[1], list->elements.size(),
                                 "Subscript");
  if (index == UINT32_MAX) return false;

  list->elements[index] = args[2];
  RETURN_VAL(args[2]);
}

DEF_PRIMITIVE(map_new)
{
  RETURN_OBJ(wrenConstruct<ObjMap>(vm, 0));
}

DEF_PRIMITIVE(map_subscript)
{
  if (!validateKey(vm, args[1])) return false;

  ObjMap* map = AS_MAP(args[0]);
  Value value = wrenMapGet(map, args[1]);
  if (IS_UNDEFINED(value)) RETURN_NULL;

  RETURN_VAL(value);
}

DEF_PRIMITIVE(map_subscriptSetter)
{
  if (!validateKey(vm, args[1])) return false;

  vm->MapSet( AS_MAP(args[0]), args[1], args[2]);
  RETURN_VAL(args[2]);
}

// Adds an entry to the map and then returns the map itself. This is called by
// the compiler when compiling map literals instead of using [_]=(_) to
// minimize stack churn.
DEF_PRIMITIVE(map_addCore)
{
  if (!validateKey(vm, args[1])) return false;
  
  vm->MapSet( AS_MAP(args[0]), args[1], args[2]);
  
  // Return the map itself.
  RETURN_VAL(args[0]);
}

DEF_PRIMITIVE(map_clear)
{
  vm->MapClear( AS_MAP(args[0]));
  RETURN_NULL;
}

DEF_PRIMITIVE(map_containsKey)
{
  if (!validateKey(vm, args[1])) return false;

  RETURN_BOOL(!IS_UNDEFINED(wrenMapGet(AS_MAP(args[0]), args[1])));
}

DEF_PRIMITIVE(map_count)
{
  RETURN_NUM(AS_MAP(args[0])->count);
}

DEF_PRIMITIVE(map_iterate)
{
  ObjMap* map = AS_MAP(args[0]);

  if (map->count == 0) RETURN_FALSE;

  // If we're starting the iteration, start at the first used entry.
  uint32_t index = 0;

  // Otherwise, start one past the last entry we stopped at.
  if (!IS_NULL(args[1]))
  {
    if (!validateInt(vm, args[1], "Iterator")) return false;

    if (AS_NUM(args[1]) < 0) RETURN_FALSE;
    index = (uint32_t)AS_NUM(args[1]);

    if (index >= map->capacity) RETURN_FALSE;

    // Advance the iterator.
    index++;
  }

  // Find a used entry, if any.
  for (; index < map->capacity; index++)
  {
    if (!IS_UNDEFINED(map->entries[index].key)) RETURN_NUM(index);
  }

  // If we get here, walked all of the entries.
  RETURN_FALSE;
}

DEF_PRIMITIVE(map_remove)
{
  if (!validateKey(vm, args[1])) return false;

  RETURN_VAL(vm->MapRemoveKey( AS_MAP(args[0]), args[1]));
}

DEF_PRIMITIVE(map_keyIteratorValue)
{
  ObjMap* map = AS_MAP(args[0]);
  uint32_t index = validateIndex(vm, args[1], map->capacity, "Iterator");
  if (index == UINT32_MAX) return false;

  MapEntry* entry = &map->entries[index];
  if (IS_UNDEFINED(entry->key))
  {
    RETURN_ERROR("Invalid map iterator.");
  }

  RETURN_VAL(entry->key);
}

DEF_PRIMITIVE(map_valueIteratorValue)
{
  ObjMap* map = AS_MAP(args[0]);
  uint32_t index = validateIndex(vm, args[1], map->capacity, "Iterator");
  if (index == UINT32_MAX) return false;

  MapEntry* entry = &map->entries[index];
  if (IS_UNDEFINED(entry->key))
  {
    RETURN_ERROR("Invalid map iterator.");
  }

  RETURN_VAL(entry->value);
}

DEF_PRIMITIVE(null_not)
{
  RETURN_VAL(TRUE_VAL);
}

DEF_PRIMITIVE(null_toString)
{
  RETURN_VAL(CONST_STRING(vm, "null"));
}

DEF_PRIMITIVE(num_fromString)
{
  if (!validateString(vm, args[1], "Argument")) return false;

  ObjString* string = AS_STRING(args[1]);

  // Corner case: Can't parse an empty string.
  if (string->length == 0) RETURN_NULL;

  errno = 0;
  char* end;
  double number = strtod(string->value, &end);

  // Skip past any trailing whitespace.
  while (*end != '\0' && isspace((unsigned char)*end)) end++;

  if (errno == ERANGE) RETURN_ERROR("Number literal is too large.");

  // We must have consumed the entire string. Otherwise, it contains non-number
  // characters and we can't parse it.
  if (end < string->value + string->length) RETURN_NULL;

  RETURN_NUM(number);
}

// Defines a primitive on Num that calls infix [op] and returns [type].
#define DEF_NUM_CONSTANT(name, value)                                          \
    DEF_PRIMITIVE(num_##name)                                                  \
    {                                                                          \
      RETURN_NUM(value);                                                       \
    }

DEF_NUM_CONSTANT(infinity, INFINITY)
DEF_NUM_CONSTANT(nan,      WREN_DOUBLE_NAN)
DEF_NUM_CONSTANT(pi,       3.14159265358979323846264338327950288)
DEF_NUM_CONSTANT(tau,      6.28318530717958647692528676655900577)

DEF_NUM_CONSTANT(largest,  DBL_MAX)
DEF_NUM_CONSTANT(smallest, DBL_MIN)

DEF_NUM_CONSTANT(maxSafeInteger, 9007199254740991.0)
DEF_NUM_CONSTANT(minSafeInteger, -9007199254740991.0)

// Defines a primitive on Num that calls infix [op] and returns [type].
#define DEF_NUM_INFIX(name, op, type)                                          \
    DEF_PRIMITIVE(num_##name)                                                  \
    {                                                                          \
      if (!validateNum(vm, args[1], "Right operand")) return false;            \
      RETURN_##type(AS_NUM(args[0]) op AS_NUM(args[1]));                       \
    }

DEF_NUM_INFIX(minus,    -,  NUM)
DEF_NUM_INFIX(plus,     +,  NUM)
DEF_NUM_INFIX(multiply, *,  NUM)
DEF_NUM_INFIX(divide,   /,  NUM)
DEF_NUM_INFIX(lt,       <,  BOOL)
DEF_NUM_INFIX(gt,       >,  BOOL)
DEF_NUM_INFIX(lte,      <=, BOOL)
DEF_NUM_INFIX(gte,      >=, BOOL)

// Defines a primitive on Num that call infix bitwise [op].
#define DEF_NUM_BITWISE(name, op)                                              \
    DEF_PRIMITIVE(num_bitwise##name)                                           \
    {                                                                          \
      if (!validateNum(vm, args[1], "Right operand")) return false;            \
      uint32_t left = (uint32_t)AS_NUM(args[0]);                               \
      uint32_t right = (uint32_t)AS_NUM(args[1]);                              \
      RETURN_NUM(left op right);                                               \
    }

DEF_NUM_BITWISE(And,        &)
DEF_NUM_BITWISE(Or,         |)
DEF_NUM_BITWISE(Xor,        ^)
DEF_NUM_BITWISE(LeftShift,  <<)
DEF_NUM_BITWISE(RightShift, >>)

// Defines a primitive method on Num that returns the result of [fn].
#define DEF_NUM_FN(name, fn)                                                   \
    DEF_PRIMITIVE(num_##name)                                                  \
    {                                                                          \
      RETURN_NUM(fn(AS_NUM(args[0])));                                         \
    }

DEF_NUM_FN(abs,     fabs)
DEF_NUM_FN(acos,    acos)
DEF_NUM_FN(asin,    asin)
DEF_NUM_FN(atan,    atan)
DEF_NUM_FN(cbrt,    cbrt)
DEF_NUM_FN(ceil,    ceil)
DEF_NUM_FN(cos,     cos)
DEF_NUM_FN(floor,   floor)
DEF_NUM_FN(negate,  -)
DEF_NUM_FN(round,   round)
DEF_NUM_FN(sin,     sin)
DEF_NUM_FN(sqrt,    sqrt)
DEF_NUM_FN(tan,     tan)
DEF_NUM_FN(log,     log)
DEF_NUM_FN(log2,    log2)
DEF_NUM_FN(exp,     exp)

DEF_PRIMITIVE(num_mod)
{
  if (!validateNum(vm, args[1], "Right operand")) return false;
  RETURN_NUM(fmod(AS_NUM(args[0]), AS_NUM(args[1])));
}

DEF_PRIMITIVE(num_eqeq)
{
  if (!IS_NUM(args[1])) RETURN_FALSE;
  RETURN_BOOL(AS_NUM(args[0]) == AS_NUM(args[1]));
}

DEF_PRIMITIVE(num_bangeq)
{
  if (!IS_NUM(args[1])) RETURN_TRUE;
  RETURN_BOOL(AS_NUM(args[0]) != AS_NUM(args[1]));
}

DEF_PRIMITIVE(num_bitwiseNot)
{
  // Bitwise operators always work on 32-bit unsigned ints.
  RETURN_NUM(~(uint32_t)AS_NUM(args[0]));
}

DEF_PRIMITIVE(num_dotDot)
{
  if (!validateNum(vm, args[1], "Right hand side of range")) return false;

  double from = AS_NUM(args[0]);
  double to = AS_NUM(args[1]);
  RETURN_VAL(OBJ_VAL(wrenConstruct<ObjRange>(vm, 0, from, to, true)));
}

DEF_PRIMITIVE(num_dotDotDot)
{
  if (!validateNum(vm, args[1], "Right hand side of range")) return false;

  double from = AS_NUM(args[0]);
  double to = AS_NUM(args[1]);
  RETURN_VAL(OBJ_VAL(wrenConstruct<ObjRange>(vm, 0, from, to, false)));
}

DEF_PRIMITIVE(num_atan2)
{
  if (!validateNum(vm, args[1], "x value")) return false;

  RETURN_NUM(atan2(AS_NUM(args[0]), AS_NUM(args[1])));
}

DEF_PRIMITIVE(num_min)
{
  if (!validateNum(vm, args[1], "Other value")) return false;

  double value = AS_NUM(args[0]);
  double other = AS_NUM(args[1]);
  RETURN_NUM(value <= other ? value : other);
}

DEF_PRIMITIVE(num_max)
{
  if (!validateNum(vm, args[1], "Other value")) return false;

  double value = AS_NUM(args[0]);
  double other = AS_NUM(args[1]);
  RETURN_NUM(value > other ? value : other);
}

DEF_PRIMITIVE(num_clamp)
{
  if (!validateNum(vm, args[1], "Min value")) return false;
  if (!validateNum(vm, args[2], "Max value")) return false;

  double value = AS_NUM(args[0]);
  double min = AS_NUM(args[1]);
  double max = AS_NUM(args[2]);
  double result = (value < min) ? min : ((value > max) ? max : value);
  RETURN_NUM(result);
}

DEF_PRIMITIVE(num_pow)
{
  if (!validateNum(vm, args[1], "Power value")) return false;

  RETURN_NUM(pow(AS_NUM(args[0]), AS_NUM(args[1])));
}

DEF_PRIMITIVE(num_fraction)
{
  double unused;
  RETURN_NUM(modf(AS_NUM(args[0]) , &unused));
}

DEF_PRIMITIVE(num_isInfinity)
{
  RETURN_BOOL(isinf(AS_NUM(args[0])));
}

DEF_PRIMITIVE(num_isInteger)
{
  double value = AS_NUM(args[0]);
  if (isnan(value) || isinf(value)) RETURN_FALSE;
  RETURN_BOOL(trunc(value) == value);
}

DEF_PRIMITIVE(num_isNan)
{
  RETURN_BOOL(isnan(AS_NUM(args[0])));
}

DEF_PRIMITIVE(num_sign)
{
  double value = AS_NUM(args[0]);
  if (value > 0)
  {
    RETURN_NUM(1);
  }
  else if (value < 0)
  {
    RETURN_NUM(-1);
  }
  else
  {
    RETURN_NUM(0);
  }
}

DEF_PRIMITIVE(num_toString)
{
  RETURN_VAL(vm->NumToString( AS_NUM(args[0])));
}

DEF_PRIMITIVE(num_truncate)
{
  double integer;
  modf(AS_NUM(args[0]) , &integer);
  RETURN_NUM(integer);
}

DEF_PRIMITIVE(object_same)
{
  RETURN_BOOL(wrenValuesEqual(args[1], args[2]));
}

DEF_PRIMITIVE(object_not)
{
  RETURN_VAL(FALSE_VAL);
}

DEF_PRIMITIVE(object_eqeq)
{
  RETURN_BOOL(wrenValuesEqual(args[0], args[1]));
}

DEF_PRIMITIVE(object_bangeq)
{
  RETURN_BOOL(!wrenValuesEqual(args[0], args[1]));
}

DEF_PRIMITIVE(object_is)
{
  if (!IS_CLASS(args[1]))
  {
    RETURN_ERROR("Right operand must be a class.");
  }

  ObjClass *classObj = vm->GetClass( args[0]);
  ObjClass *baseClassObj = AS_CLASS(args[1]);

  // Walk the superclass chain looking for the class.
  do
  {
    if (baseClassObj == classObj) RETURN_BOOL(true);

    classObj = classObj->superclass;
  }
  while (classObj != nullptr);

  RETURN_BOOL(false);
}

DEF_PRIMITIVE(object_toString)
{
  Obj* obj = AS_OBJ(args[0]);
  Value name = OBJ_VAL(obj->classObj->name);
  RETURN_VAL(vm->StringFormat( "instance of @", name));
}

DEF_PRIMITIVE(object_type)
{
  RETURN_OBJ(vm->GetClass( args[0]));
}

DEF_PRIMITIVE(range_from)
{
  RETURN_NUM(AS_RANGE(args[0])->from);
}

DEF_PRIMITIVE(range_to)
{
  RETURN_NUM(AS_RANGE(args[0])->to);
}

DEF_PRIMITIVE(range_min)
{
  ObjRange* range = AS_RANGE(args[0]);
  RETURN_NUM(fmin(range->from, range->to));
}

DEF_PRIMITIVE(range_max)
{
  ObjRange* range = AS_RANGE(args[0]);
  RETURN_NUM(fmax(range->from, range->to));
}

DEF_PRIMITIVE(range_isInclusive)
{
  RETURN_BOOL(AS_RANGE(args[0])->isInclusive);
}

DEF_PRIMITIVE(range_iterate)
{
  ObjRange* range = AS_RANGE(args[0]);

  // Special case: empty range.
  if (range->from == range->to && !range->isInclusive) RETURN_FALSE;

  // Start the iteration.
  if (IS_NULL(args[1])) RETURN_NUM(range->from);

  if (!validateNum(vm, args[1], "Iterator")) return false;

  double iterator = AS_NUM(args[1]);

  // Iterate towards [to] from [from].
  if (range->from < range->to)
  {
    iterator++;
    if (iterator > range->to) RETURN_FALSE;
  }
  else
  {
    iterator--;
    if (iterator < range->to) RETURN_FALSE;
  }

  if (!range->isInclusive && iterator == range->to) RETURN_FALSE;

  RETURN_NUM(iterator);
}

DEF_PRIMITIVE(range_iteratorValue)
{
  // Assume the iterator is a number so that is the value of the range.
  RETURN_VAL(args[1]);
}

DEF_PRIMITIVE(range_toString)
{
  ObjRange* range = AS_RANGE(args[0]);

  Value from = vm->NumToString( range->from);
  vm->PushRoot( AS_OBJ(from));

  Value to = vm->NumToString( range->to);
  vm->PushRoot( AS_OBJ(to));

  Value result = vm->StringFormat( "@$@", from,
                                  range->isInclusive ? ".." : "...", to);

  vm->PopRoot();
  vm->PopRoot();
  RETURN_VAL(result);
}

DEF_PRIMITIVE(string_fromCodePoint)
{
  if (!validateInt(vm, args[1], "Code point")) return false;

  int codePoint = (int)AS_NUM(args[1]);
  if (codePoint < 0)
  {
    RETURN_ERROR("Code point cannot be negative.");
  }
  else if (codePoint > 0x10ffff)
  {
    RETURN_ERROR("Code point cannot be greater than 0x10ffff.");
  }

  RETURN_VAL(vm->StringFromCodePoint( codePoint));
}

DEF_PRIMITIVE(string_fromByte)
{
  if (!validateInt(vm, args[1], "Byte")) return false;
  int byte = (int) AS_NUM(args[1]);
  if (byte < 0)
  {
    RETURN_ERROR("Byte cannot be negative.");
  }
  else if (byte > 0xff)
  {
    RETURN_ERROR("Byte cannot be greater than 0xff.");
  }
  RETURN_VAL(vm->StringFromByte( (uint8_t) byte));
}

DEF_PRIMITIVE(string_byteAt)
{
  ObjString* string = AS_STRING(args[0]);

  uint32_t index = validateIndex(vm, args[1], string->length, "Index");
  if (index == UINT32_MAX) return false;

  RETURN_NUM((uint8_t)string->value[index]);
}

DEF_PRIMITIVE(string_byteCount)
{
  RETURN_NUM(AS_STRING(args[0])->length);
}

DEF_PRIMITIVE(string_codePointAt)
{
  ObjString* string = AS_STRING(args[0]);

  uint32_t index = validateIndex(vm, args[1], string->length, "Index");
  if (index == UINT32_MAX) return false;

  // If we are in the middle of a UTF-8 sequence, indicate that.
  const uint8_t* bytes = (uint8_t*)string->value;
  if ((bytes[index] & 0xc0) == 0x80) RETURN_NUM(-1);

  // Decode the UTF-8 sequence.
  RETURN_NUM(wrenUtf8Decode((uint8_t*)string->value + index,
                            string->length - index));
}

DEF_PRIMITIVE(string_contains)
{
  if (!validateString(vm, args[1], "Argument")) return false;

  ObjString* string = AS_STRING(args[0]);
  ObjString* search = AS_STRING(args[1]);

  RETURN_BOOL(wrenStringFind(string, search, 0) != UINT32_MAX);
}

DEF_PRIMITIVE(string_endsWith)
{
  if (!validateString(vm, args[1], "Argument")) return false;

  ObjString* string = AS_STRING(args[0]);
  ObjString* search = AS_STRING(args[1]);

  // Edge case: If the search string is longer then return false right away.
  if (search->length > string->length) RETURN_FALSE;

  RETURN_BOOL(memcmp(string->value + string->length - search->length,
                     search->value, search->length) == 0);
}

DEF_PRIMITIVE(string_indexOf1)
{
  if (!validateString(vm, args[1], "Argument")) return false;

  ObjString* string = AS_STRING(args[0]);
  ObjString* search = AS_STRING(args[1]);

  uint32_t index = wrenStringFind(string, search, 0);
  RETURN_NUM(index == UINT32_MAX ? -1 : (int)index);
}

DEF_PRIMITIVE(string_indexOf2)
{
  if (!validateString(vm, args[1], "Argument")) return false;

  ObjString* string = AS_STRING(args[0]);
  ObjString* search = AS_STRING(args[1]);
  uint32_t start = validateIndex(vm, args[2], string->length, "Start");
  if (start == UINT32_MAX) return false;
  
  uint32_t index = wrenStringFind(string, search, start);
  RETURN_NUM(index == UINT32_MAX ? -1 : (int)index);
}

DEF_PRIMITIVE(string_iterate)
{
  ObjString* string = AS_STRING(args[0]);

  // If we're starting the iteration, return the first index.
  if (IS_NULL(args[1]))
  {
    if (string->length == 0) RETURN_FALSE;
    RETURN_NUM(0);
  }

  if (!validateInt(vm, args[1], "Iterator")) return false;

  if (AS_NUM(args[1]) < 0) RETURN_FALSE;
  uint32_t index = (uint32_t)AS_NUM(args[1]);

  // Advance to the beginning of the next UTF-8 sequence.
  do
  {
    index++;
    if (index >= string->length) RETURN_FALSE;
  } while ((string->value[index] & 0xc0) == 0x80);

  RETURN_NUM(index);
}

DEF_PRIMITIVE(string_iterateByte)
{
  ObjString* string = AS_STRING(args[0]);

  // If we're starting the iteration, return the first index.
  if (IS_NULL(args[1]))
  {
    if (string->length == 0) RETURN_FALSE;
    RETURN_NUM(0);
  }

  if (!validateInt(vm, args[1], "Iterator")) return false;

  if (AS_NUM(args[1]) < 0) RETURN_FALSE;
  uint32_t index = (uint32_t)AS_NUM(args[1]);

  // Advance to the next byte.
  index++;
  if (index >= string->length) RETURN_FALSE;

  RETURN_NUM(index);
}

DEF_PRIMITIVE(string_iteratorValue)
{
  ObjString* string = AS_STRING(args[0]);
  uint32_t index = validateIndex(vm, args[1], string->length, "Iterator");
  if (index == UINT32_MAX) return false;

  RETURN_VAL(vm->StringCodePointAt( string, index));
}

DEF_PRIMITIVE(string_startsWith)
{
  if (!validateString(vm, args[1], "Argument")) return false;

  ObjString* string = AS_STRING(args[0]);
  ObjString* search = AS_STRING(args[1]);

  // Edge case: If the search string is longer then return false right away.
  if (search->length > string->length) RETURN_FALSE;

  RETURN_BOOL(memcmp(string->value, search->value, search->length) == 0);
}

DEF_PRIMITIVE(string_plus)
{
  if (!validateString(vm, args[1], "Right operand")) return false;
  RETURN_VAL(vm->StringFormat( "@@", args[0], args[1]));
}

DEF_PRIMITIVE(string_subscript)
{
  ObjString* string = AS_STRING(args[0]);

  if (IS_NUM(args[1]))
  {
    int index = validateIndex(vm, args[1], string->length, "Subscript");
    if (index == -1) return false;

    RETURN_VAL(vm->StringCodePointAt( string, index));
  }

  if (!IS_RANGE(args[1]))
  {
    RETURN_ERROR("Subscript must be a number or a range.");
  }

  int step;
  uint32_t count = string->length;
  int start = calculateRange(vm, AS_RANGE(args[1]), &count, &step);
  if (start == -1) return false;

  RETURN_VAL(vm->NewStringFromRange( string, start, count, step));
}

DEF_PRIMITIVE(string_toString)
{
  RETURN_VAL(args[0]);
}

DEF_PRIMITIVE(system_clock)
{
  RETURN_NUM((double)clock() / CLOCKS_PER_SEC);
}

DEF_PRIMITIVE(system_gc)
{
  wrenCollectGarbage(vm);
  RETURN_NULL;
}

DEF_PRIMITIVE(system_writeString)
{
  vm->Write(AS_CSTRING(args[1]));

  RETURN_VAL(args[1]);
}

// Creates either the Object or Class class in the core module with [name].
ObjClass* WrenVM::DefineClass(ObjModule* module, const char* name)
{
  ObjString* nameString = AS_STRING(this->NewString( name));
  this->PushRoot( (Obj*)nameString);

  ObjClass* classObj = this->NewSingleClass( 0, nameString);

  this->DefineVariable( module, name, nameString->length, OBJ_VAL(classObj), nullptr);

  this->PopRoot();
  return classObj;
}

void WrenVM::InitializeCore()
{
  // Alias so the PRIMITIVE() macro -- written for the fn(WrenVM*, Value*)
  // primitive signature -- works in this member.
  WrenVM* vm = this;

  ObjModule* coreModule = wrenConstruct<ObjModule>(this, 0, nullptr);
  this->PushRoot( (Obj*)coreModule);
  
  // The core module's key is null in the module map.
  this->MapSet( this->modules, NULL_VAL, OBJ_VAL(coreModule));
  this->PopRoot(); // coreModule.

  // Define the root Object class. This has to be done a little specially
  // because it has no superclass.
  this->objectClass = this->DefineClass(coreModule, "Object");
  PRIMITIVE(this->objectClass, "!", object_not);
  PRIMITIVE(this->objectClass, "==(_)", object_eqeq);
  PRIMITIVE(this->objectClass, "!=(_)", object_bangeq);
  PRIMITIVE(this->objectClass, "is(_)", object_is);
  PRIMITIVE(this->objectClass, "toString", object_toString);
  PRIMITIVE(this->objectClass, "type", object_type);

  // Now we can define Class, which is a subclass of Object.
  this->classClass = this->DefineClass(coreModule, "Class");
  this->BindSuperclass( this->classClass, this->objectClass);
  PRIMITIVE(this->classClass, "name", class_name);
  PRIMITIVE(this->classClass, "supertype", class_supertype);
  PRIMITIVE(this->classClass, "toString", class_toString);
  PRIMITIVE(this->classClass, "attributes", class_attributes);

  // Finally, we can define Object's metaclass which is a subclass of Class.
  ObjClass* objectMetaclass = this->DefineClass(coreModule, "Object metaclass");

  // Wire up the metaclass relationships now that all three classes are built.
  this->objectClass->classObj = objectMetaclass;
  objectMetaclass->classObj = this->classClass;
  this->classClass->classObj = this->classClass;

  // Do this after wiring up the metaclasses so objectMetaclass doesn't get
  // collected.
  this->BindSuperclass( objectMetaclass, this->classClass);

  PRIMITIVE(objectMetaclass, "same(_,_)", object_same);

  // The core class diagram ends up looking like this, where single lines point
  // to a class's superclass, and double lines point to its metaclass:
  //
  //        .------------------------------------. .====.
  //        |                  .---------------. | #    #
  //        v                  |               v | v    #
  //   .---------.   .-------------------.   .-------.  #
  //   | Object  |==>| Object metaclass  |==>| Class |=="
  //   '---------'   '-------------------'   '-------'
  //        ^                                 ^ ^ ^ ^
  //        |                  .--------------' # | #
  //        |                  |                # | #
  //   .---------.   .-------------------.      # | # -.
  //   |  Base   |==>|  Base metaclass   |======" | #  |
  //   '---------'   '-------------------'        | #  |
  //        ^                                     | #  |
  //        |                  .------------------' #  | Example classes
  //        |                  |                    #  |
  //   .---------.   .-------------------.          #  |
  //   | Derived |==>| Derived metaclass |=========="  |
  //   '---------'   '-------------------'            -'

  // The rest of the classes can now be defined normally.
  wrenInterpret(this, nullptr, coreModuleSource);

  this->boolClass = AS_CLASS(this->FindVariable( coreModule, "Bool"));
  PRIMITIVE(this->boolClass, "toString", bool_toString);
  PRIMITIVE(this->boolClass, "!", bool_not);

  this->fiberClass = AS_CLASS(this->FindVariable( coreModule, "Fiber"));
  PRIMITIVE(this->fiberClass->classObj, "new(_)", fiber_new);
  PRIMITIVE(this->fiberClass->classObj, "abort(_)", fiber_abort);
  PRIMITIVE(this->fiberClass->classObj, "exit(_)", fiber_exit);
  PRIMITIVE(this->fiberClass->classObj, "current", fiber_current);
  PRIMITIVE(this->fiberClass->classObj, "suspend()", fiber_suspend);
  PRIMITIVE(this->fiberClass->classObj, "yield()", fiber_yield);
  PRIMITIVE(this->fiberClass->classObj, "yield(_)", fiber_yield1);
  PRIMITIVE(this->fiberClass, "call()", fiber_call);
  PRIMITIVE(this->fiberClass, "call(_)", fiber_call1);
  PRIMITIVE(this->fiberClass, "error", fiber_error);
  PRIMITIVE(this->fiberClass, "isDone", fiber_isDone);
  PRIMITIVE(this->fiberClass, "transfer()", fiber_transfer);
  PRIMITIVE(this->fiberClass, "transfer(_)", fiber_transfer1);
  PRIMITIVE(this->fiberClass, "transferError(_)", fiber_transferError);
  PRIMITIVE(this->fiberClass, "try()", fiber_try);
  PRIMITIVE(this->fiberClass, "try(_)", fiber_try1);

  this->fnClass = AS_CLASS(this->FindVariable( coreModule, "Fn"));
  PRIMITIVE(this->fnClass->classObj, "new(_)", fn_new);

  PRIMITIVE(this->fnClass, "arity", fn_arity);

  FUNCTION_CALL(this->fnClass, "call()", fn_call0);
  FUNCTION_CALL(this->fnClass, "call(_)", fn_call1);
  FUNCTION_CALL(this->fnClass, "call(_,_)", fn_call2);
  FUNCTION_CALL(this->fnClass, "call(_,_,_)", fn_call3);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_)", fn_call4);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_)", fn_call5);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_)", fn_call6);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_)", fn_call7);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_)", fn_call8);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_)", fn_call9);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_)", fn_call10);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_)", fn_call11);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_,_)", fn_call12);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_,_,_)", fn_call13);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_)", fn_call14);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)", fn_call15);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)", fn_call16);
  
  PRIMITIVE(this->fnClass, "toString", fn_toString);

  this->nullClass = AS_CLASS(this->FindVariable( coreModule, "Null"));
  PRIMITIVE(this->nullClass, "!", null_not);
  PRIMITIVE(this->nullClass, "toString", null_toString);

  this->numClass = AS_CLASS(this->FindVariable( coreModule, "Num"));
  PRIMITIVE(this->numClass->classObj, "fromString(_)", num_fromString);
  PRIMITIVE(this->numClass->classObj, "infinity", num_infinity);
  PRIMITIVE(this->numClass->classObj, "nan", num_nan);
  PRIMITIVE(this->numClass->classObj, "pi", num_pi);
  PRIMITIVE(this->numClass->classObj, "tau", num_tau);
  PRIMITIVE(this->numClass->classObj, "largest", num_largest);
  PRIMITIVE(this->numClass->classObj, "smallest", num_smallest);
  PRIMITIVE(this->numClass->classObj, "maxSafeInteger", num_maxSafeInteger);
  PRIMITIVE(this->numClass->classObj, "minSafeInteger", num_minSafeInteger);
  PRIMITIVE(this->numClass, "-(_)", num_minus);
  PRIMITIVE(this->numClass, "+(_)", num_plus);
  PRIMITIVE(this->numClass, "*(_)", num_multiply);
  PRIMITIVE(this->numClass, "/(_)", num_divide);
  PRIMITIVE(this->numClass, "<(_)", num_lt);
  PRIMITIVE(this->numClass, ">(_)", num_gt);
  PRIMITIVE(this->numClass, "<=(_)", num_lte);
  PRIMITIVE(this->numClass, ">=(_)", num_gte);
  PRIMITIVE(this->numClass, "&(_)", num_bitwiseAnd);
  PRIMITIVE(this->numClass, "|(_)", num_bitwiseOr);
  PRIMITIVE(this->numClass, "^(_)", num_bitwiseXor);
  PRIMITIVE(this->numClass, "<<(_)", num_bitwiseLeftShift);
  PRIMITIVE(this->numClass, ">>(_)", num_bitwiseRightShift);
  PRIMITIVE(this->numClass, "abs", num_abs);
  PRIMITIVE(this->numClass, "acos", num_acos);
  PRIMITIVE(this->numClass, "asin", num_asin);
  PRIMITIVE(this->numClass, "atan", num_atan);
  PRIMITIVE(this->numClass, "cbrt", num_cbrt);
  PRIMITIVE(this->numClass, "ceil", num_ceil);
  PRIMITIVE(this->numClass, "cos", num_cos);
  PRIMITIVE(this->numClass, "floor", num_floor);
  PRIMITIVE(this->numClass, "-", num_negate);
  PRIMITIVE(this->numClass, "round", num_round);
  PRIMITIVE(this->numClass, "min(_)", num_min);
  PRIMITIVE(this->numClass, "max(_)", num_max);
  PRIMITIVE(this->numClass, "clamp(_,_)", num_clamp);
  PRIMITIVE(this->numClass, "sin", num_sin);
  PRIMITIVE(this->numClass, "sqrt", num_sqrt);
  PRIMITIVE(this->numClass, "tan", num_tan);
  PRIMITIVE(this->numClass, "log", num_log);
  PRIMITIVE(this->numClass, "log2", num_log2);
  PRIMITIVE(this->numClass, "exp", num_exp);
  PRIMITIVE(this->numClass, "%(_)", num_mod);
  PRIMITIVE(this->numClass, "~", num_bitwiseNot);
  PRIMITIVE(this->numClass, "..(_)", num_dotDot);
  PRIMITIVE(this->numClass, "...(_)", num_dotDotDot);
  PRIMITIVE(this->numClass, "atan(_)", num_atan2);
  PRIMITIVE(this->numClass, "pow(_)", num_pow);
  PRIMITIVE(this->numClass, "fraction", num_fraction);
  PRIMITIVE(this->numClass, "isInfinity", num_isInfinity);
  PRIMITIVE(this->numClass, "isInteger", num_isInteger);
  PRIMITIVE(this->numClass, "isNan", num_isNan);
  PRIMITIVE(this->numClass, "sign", num_sign);
  PRIMITIVE(this->numClass, "toString", num_toString);
  PRIMITIVE(this->numClass, "truncate", num_truncate);

  // These are defined just so that 0 and -0 are equal, which is specified by
  // IEEE 754 even though they have different bit representations.
  PRIMITIVE(this->numClass, "==(_)", num_eqeq);
  PRIMITIVE(this->numClass, "!=(_)", num_bangeq);

  this->stringClass = AS_CLASS(this->FindVariable( coreModule, "String"));
  PRIMITIVE(this->stringClass->classObj, "fromCodePoint(_)", string_fromCodePoint);
  PRIMITIVE(this->stringClass->classObj, "fromByte(_)", string_fromByte);
  PRIMITIVE(this->stringClass, "+(_)", string_plus);
  PRIMITIVE(this->stringClass, "[_]", string_subscript);
  PRIMITIVE(this->stringClass, "byteAt_(_)", string_byteAt);
  PRIMITIVE(this->stringClass, "byteCount_", string_byteCount);
  PRIMITIVE(this->stringClass, "codePointAt_(_)", string_codePointAt);
  PRIMITIVE(this->stringClass, "contains(_)", string_contains);
  PRIMITIVE(this->stringClass, "endsWith(_)", string_endsWith);
  PRIMITIVE(this->stringClass, "indexOf(_)", string_indexOf1);
  PRIMITIVE(this->stringClass, "indexOf(_,_)", string_indexOf2);
  PRIMITIVE(this->stringClass, "iterate(_)", string_iterate);
  PRIMITIVE(this->stringClass, "iterateByte_(_)", string_iterateByte);
  PRIMITIVE(this->stringClass, "iteratorValue(_)", string_iteratorValue);
  PRIMITIVE(this->stringClass, "startsWith(_)", string_startsWith);
  PRIMITIVE(this->stringClass, "toString", string_toString);

  this->listClass = AS_CLASS(this->FindVariable( coreModule, "List"));
  PRIMITIVE(this->listClass->classObj, "filled(_,_)", list_filled);
  PRIMITIVE(this->listClass->classObj, "new()", list_new);
  PRIMITIVE(this->listClass, "[_]", list_subscript);
  PRIMITIVE(this->listClass, "[_]=(_)", list_subscriptSetter);
  PRIMITIVE(this->listClass, "add(_)", list_add);
  PRIMITIVE(this->listClass, "addCore_(_)", list_addCore);
  PRIMITIVE(this->listClass, "clear()", list_clear);
  PRIMITIVE(this->listClass, "count", list_count);
  PRIMITIVE(this->listClass, "insert(_,_)", list_insert);
  PRIMITIVE(this->listClass, "iterate(_)", list_iterate);
  PRIMITIVE(this->listClass, "iteratorValue(_)", list_iteratorValue);
  PRIMITIVE(this->listClass, "removeAt(_)", list_removeAt);
  PRIMITIVE(this->listClass, "swap(_,_)", list_swap);

  this->mapClass = AS_CLASS(this->FindVariable( coreModule, "Map"));
  PRIMITIVE(this->mapClass->classObj, "new()", map_new);
  PRIMITIVE(this->mapClass, "[_]", map_subscript);
  PRIMITIVE(this->mapClass, "[_]=(_)", map_subscriptSetter);
  PRIMITIVE(this->mapClass, "addCore_(_,_)", map_addCore);
  PRIMITIVE(this->mapClass, "clear()", map_clear);
  PRIMITIVE(this->mapClass, "containsKey(_)", map_containsKey);
  PRIMITIVE(this->mapClass, "count", map_count);
  PRIMITIVE(this->mapClass, "remove(_)", map_remove);
  PRIMITIVE(this->mapClass, "iterate(_)", map_iterate);
  PRIMITIVE(this->mapClass, "keyIteratorValue_(_)", map_keyIteratorValue);
  PRIMITIVE(this->mapClass, "valueIteratorValue_(_)", map_valueIteratorValue);

  this->rangeClass = AS_CLASS(this->FindVariable( coreModule, "Range"));
  PRIMITIVE(this->rangeClass, "from", range_from);
  PRIMITIVE(this->rangeClass, "to", range_to);
  PRIMITIVE(this->rangeClass, "min", range_min);
  PRIMITIVE(this->rangeClass, "max", range_max);
  PRIMITIVE(this->rangeClass, "isInclusive", range_isInclusive);
  PRIMITIVE(this->rangeClass, "iterate(_)", range_iterate);
  PRIMITIVE(this->rangeClass, "iteratorValue(_)", range_iteratorValue);
  PRIMITIVE(this->rangeClass, "toString", range_toString);

  ObjClass* systemClass = AS_CLASS(this->FindVariable( coreModule, "System"));
  PRIMITIVE(systemClass->classObj, "clock", system_clock);
  PRIMITIVE(systemClass->classObj, "gc()", system_gc);
  PRIMITIVE(systemClass->classObj, "writeString_(_)", system_writeString);

  // While bootstrapping the core types and running the core module, a number
  // of string objects have been created, many of which were instantiated
  // before stringClass was stored in the VM. Some of them *must* be created
  // first -- the ObjClass for string itself has a reference to the ObjString
  // for its name.
  //
  // These all currently have a NULL classObj pointer, so go back and assign
  // them now that the string class is known.
  for (Obj* obj = this->first; obj != nullptr; obj = obj->next)
  {
    if (obj->type == OBJ_STRING) obj->classObj = this->stringClass;
  }
}
