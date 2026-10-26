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

DEF_PRIMITIVE(BoolNot)
{
  RETURN_BOOL(!AS_BOOL(args[0]));
}

DEF_PRIMITIVE(BoolToString)
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

DEF_PRIMITIVE(ClassName)
{
  RETURN_OBJ(AS_CLASS(args[0])->name);
}

DEF_PRIMITIVE(ClassSupertype)
{
  ObjClass* classObj = AS_CLASS(args[0]);

  // Object has no superclass.
  if (classObj->superclass == nullptr) RETURN_NULL;

  RETURN_OBJ(classObj->superclass);
}

DEF_PRIMITIVE(ClassToString)
{
  RETURN_OBJ(AS_CLASS(args[0])->name);
}

DEF_PRIMITIVE(ClassAttributes)
{
  RETURN_VAL(AS_CLASS(args[0])->attributes);
}

DEF_PRIMITIVE(FiberNew)
{
  if (!validateFn(vm, args[1], "Argument")) return false;

  ObjClosure* closure = AS_CLOSURE(args[1]);
  if (closure->fn->arity > 1)
  {
    RETURN_ERROR("Function cannot take more than one parameter.");
  }
  
  RETURN_OBJ(wrenConstruct<ObjFiber>(vm, 0, closure));
}

DEF_PRIMITIVE(FiberAbort)
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

DEF_PRIMITIVE(FiberCall)
{
  return vm->RunFiber(AS_FIBER(args[0]), args, true, false, "call");
}

DEF_PRIMITIVE(FiberCall1)
{
  return vm->RunFiber(AS_FIBER(args[0]), args, true, true, "call");
}

DEF_PRIMITIVE(FiberCurrent)
{
  RETURN_OBJ(vm->Fiber());
}

DEF_PRIMITIVE(FiberError)
{
  RETURN_VAL(AS_FIBER(args[0])->error);
}

DEF_PRIMITIVE(FiberExit)
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

DEF_PRIMITIVE(FiberIsDone)
{
  ObjFiber* runFiber = AS_FIBER(args[0]);
  RETURN_BOOL(runFiber->numFrames == 0 || wrenHasError(runFiber));
}

DEF_PRIMITIVE(FiberSuspend)
{
  // Switching to a null fiber tells the interpreter to stop and exit.
  vm->SetFiber(nullptr);
  vm->SetApiStack(nullptr);
  return false;
}

DEF_PRIMITIVE(FiberTransfer)
{
  return vm->RunFiber(AS_FIBER(args[0]), args, false, false, "transfer to");
}

DEF_PRIMITIVE(FiberTransfer1)
{
  return vm->RunFiber(AS_FIBER(args[0]), args, false, true, "transfer to");
}

DEF_PRIMITIVE(FiberTransferError)
{
  vm->RunFiber(AS_FIBER(args[0]), args, false, true, "transfer to");
  vm->Fiber()->error = args[1];
  return false;
}

DEF_PRIMITIVE(FiberTry)
{
  vm->RunFiber(AS_FIBER(args[0]), args, true, false, "try");
  
  // If we're switching to a valid fiber to try, remember that we're trying it.
  if (!wrenHasError(vm->Fiber())) vm->Fiber()->state = FIBER_TRY;
  return false;
}

DEF_PRIMITIVE(FiberTry1)
{
  vm->RunFiber(AS_FIBER(args[0]), args, true, true, "try");
  
  // If we're switching to a valid fiber to try, remember that we're trying it.
  if (!wrenHasError(vm->Fiber())) vm->Fiber()->state = FIBER_TRY;
  return false;
}

DEF_PRIMITIVE(FiberYield)
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

DEF_PRIMITIVE(FiberYield1)
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

DEF_PRIMITIVE(FnNew)
{
  if (!validateFn(vm, args[1], "Argument")) return false;

  // The block argument is already a function, so just return it.
  RETURN_VAL(args[1]);
}

DEF_PRIMITIVE(FnArity)
{
  RETURN_NUM(AS_CLOSURE(args[0])->fn->arity);
}

void WrenVM::CallFn(Value* args, int numArgs)
{
  // +1 to include the function itself.
  this->CallFunction( this->fiber, AS_CLOSURE(args[0]), numArgs + 1);
}

#define DEF_FN_CALL(numArgs)                                                   \
    DEF_PRIMITIVE(FnCall##numArgs)                                            \
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

DEF_PRIMITIVE(FnToString)
{
  RETURN_VAL(CONST_STRING(vm, "<fn>"));
}

// Creates a new list of size args[1], with all elements initialized to args[2].
DEF_PRIMITIVE(ListFilled)
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

DEF_PRIMITIVE(ListNew)
{
  RETURN_OBJ(wrenConstruct<ObjList>(vm, 0, 0));
}

DEF_PRIMITIVE(ListAdd)
{
  AS_LIST(args[0])->elements.push_back(args[1]);
  RETURN_VAL(args[1]);
}

// Adds an element to the list and then returns the list itself. This is called
// by the compiler when compiling list literals instead of using add() to
// minimize stack churn.
DEF_PRIMITIVE(ListAddCore)
{
  AS_LIST(args[0])->elements.push_back(args[1]);
  
  // Return the list.
  RETURN_VAL(args[0]);
}

DEF_PRIMITIVE(ListClear)
{
  AS_LIST(args[0])->elements.clear();
  RETURN_NULL;
}

DEF_PRIMITIVE(ListCount)
{
  RETURN_NUM(AS_LIST(args[0])->elements.size());
}

DEF_PRIMITIVE(ListInsert)
{
  ObjList* list = AS_LIST(args[0]);

  // count + 1 here so you can "insert" at the very end.
  uint32_t index = validateIndex(vm, args[1], list->elements.size() + 1,
                                 "Index");
  if (index == UINT32_MAX) return false;

  vm->ListInsert( list, args[2], index);
  RETURN_VAL(args[2]);
}

DEF_PRIMITIVE(ListIterate)
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

DEF_PRIMITIVE(ListIteratorValue)
{
  ObjList* list = AS_LIST(args[0]);
  uint32_t index = validateIndex(vm, args[1], list->elements.size(), "Iterator");
  if (index == UINT32_MAX) return false;

  RETURN_VAL(list->elements[index]);
}

DEF_PRIMITIVE(ListRemoveAt)
{
  ObjList* list = AS_LIST(args[0]);
  uint32_t index = validateIndex(vm, args[1], list->elements.size(), "Index");
  if (index == UINT32_MAX) return false;

  RETURN_VAL(vm->ListRemoveAt( list, index));
}

DEF_PRIMITIVE(ListSwap)
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

DEF_PRIMITIVE(ListSubscript)
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

DEF_PRIMITIVE(ListSubscriptSetter)
{
  ObjList* list = AS_LIST(args[0]);
  uint32_t index = validateIndex(vm, args[1], list->elements.size(),
                                 "Subscript");
  if (index == UINT32_MAX) return false;

  list->elements[index] = args[2];
  RETURN_VAL(args[2]);
}

DEF_PRIMITIVE(MapNew)
{
  RETURN_OBJ(wrenConstruct<ObjMap>(vm, 0));
}

DEF_PRIMITIVE(MapSubscript)
{
  if (!validateKey(vm, args[1])) return false;

  ObjMap* map = AS_MAP(args[0]);
  Value value = wrenMapGet(map, args[1]);
  if (IS_UNDEFINED(value)) RETURN_NULL;

  RETURN_VAL(value);
}

DEF_PRIMITIVE(MapSubscriptSetter)
{
  if (!validateKey(vm, args[1])) return false;

  vm->MapSet( AS_MAP(args[0]), args[1], args[2]);
  RETURN_VAL(args[2]);
}

// Adds an entry to the map and then returns the map itself. This is called by
// the compiler when compiling map literals instead of using [_]=(_) to
// minimize stack churn.
DEF_PRIMITIVE(MapAddCore)
{
  if (!validateKey(vm, args[1])) return false;
  
  vm->MapSet( AS_MAP(args[0]), args[1], args[2]);
  
  // Return the map itself.
  RETURN_VAL(args[0]);
}

DEF_PRIMITIVE(MapClear)
{
  vm->MapClear( AS_MAP(args[0]));
  RETURN_NULL;
}

DEF_PRIMITIVE(MapContainsKey)
{
  if (!validateKey(vm, args[1])) return false;

  RETURN_BOOL(!IS_UNDEFINED(wrenMapGet(AS_MAP(args[0]), args[1])));
}

DEF_PRIMITIVE(MapCount)
{
  RETURN_NUM(AS_MAP(args[0])->count);
}

DEF_PRIMITIVE(MapIterate)
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

DEF_PRIMITIVE(MapRemove)
{
  if (!validateKey(vm, args[1])) return false;

  RETURN_VAL(vm->MapRemoveKey( AS_MAP(args[0]), args[1]));
}

DEF_PRIMITIVE(MapKeyIteratorValue)
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

DEF_PRIMITIVE(MapValueIteratorValue)
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

DEF_PRIMITIVE(NullNot)
{
  RETURN_VAL(TRUE_VAL);
}

DEF_PRIMITIVE(NullToString)
{
  RETURN_VAL(CONST_STRING(vm, "null"));
}

DEF_PRIMITIVE(NumFromString)
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
    DEF_PRIMITIVE(Num##name)                                                  \
    {                                                                          \
      RETURN_NUM(value);                                                       \
    }

DEF_NUM_CONSTANT(Infinity, INFINITY)
DEF_NUM_CONSTANT(Nan,      WREN_DOUBLE_NAN)
DEF_NUM_CONSTANT(Pi,       3.14159265358979323846264338327950288)
DEF_NUM_CONSTANT(Tau,      6.28318530717958647692528676655900577)

DEF_NUM_CONSTANT(Largest,  DBL_MAX)
DEF_NUM_CONSTANT(Smallest, DBL_MIN)

DEF_NUM_CONSTANT(MaxSafeInteger, 9007199254740991.0)
DEF_NUM_CONSTANT(MinSafeInteger, -9007199254740991.0)

// Defines a primitive on Num that calls infix [op] and returns [type].
#define DEF_NUM_INFIX(name, op, type)                                          \
    DEF_PRIMITIVE(Num##name)                                                  \
    {                                                                          \
      if (!validateNum(vm, args[1], "Right operand")) return false;            \
      RETURN_##type(AS_NUM(args[0]) op AS_NUM(args[1]));                       \
    }

DEF_NUM_INFIX(Minus,    -,  NUM)
DEF_NUM_INFIX(Plus,     +,  NUM)
DEF_NUM_INFIX(Multiply, *,  NUM)
DEF_NUM_INFIX(Divide,   /,  NUM)
DEF_NUM_INFIX(Lt,       <,  BOOL)
DEF_NUM_INFIX(Gt,       >,  BOOL)
DEF_NUM_INFIX(Lte,      <=, BOOL)
DEF_NUM_INFIX(Gte,      >=, BOOL)

// Defines a primitive on Num that call infix bitwise [op].
#define DEF_NUM_BITWISE(name, op)                                              \
    DEF_PRIMITIVE(NumBitwise##name)                                           \
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
    DEF_PRIMITIVE(Num##name)                                                  \
    {                                                                          \
      RETURN_NUM(fn(AS_NUM(args[0])));                                         \
    }

DEF_NUM_FN(Abs,     fabs)
DEF_NUM_FN(Acos,    acos)
DEF_NUM_FN(Asin,    asin)
DEF_NUM_FN(Atan,    atan)
DEF_NUM_FN(Cbrt,    cbrt)
DEF_NUM_FN(Ceil,    ceil)
DEF_NUM_FN(Cos,     cos)
DEF_NUM_FN(Floor,   floor)
DEF_NUM_FN(Negate,  -)
DEF_NUM_FN(Round,   round)
DEF_NUM_FN(Sin,     sin)
DEF_NUM_FN(Sqrt,    sqrt)
DEF_NUM_FN(Tan,     tan)
DEF_NUM_FN(Log,     log)
DEF_NUM_FN(Log2,    log2)
DEF_NUM_FN(Exp,     exp)

DEF_PRIMITIVE(NumMod)
{
  if (!validateNum(vm, args[1], "Right operand")) return false;
  RETURN_NUM(fmod(AS_NUM(args[0]), AS_NUM(args[1])));
}

DEF_PRIMITIVE(NumEqeq)
{
  if (!IS_NUM(args[1])) RETURN_FALSE;
  RETURN_BOOL(AS_NUM(args[0]) == AS_NUM(args[1]));
}

DEF_PRIMITIVE(NumBangeq)
{
  if (!IS_NUM(args[1])) RETURN_TRUE;
  RETURN_BOOL(AS_NUM(args[0]) != AS_NUM(args[1]));
}

DEF_PRIMITIVE(NumBitwiseNot)
{
  // Bitwise operators always work on 32-bit unsigned ints.
  RETURN_NUM(~(uint32_t)AS_NUM(args[0]));
}

DEF_PRIMITIVE(NumDotDot)
{
  if (!validateNum(vm, args[1], "Right hand side of range")) return false;

  double from = AS_NUM(args[0]);
  double to = AS_NUM(args[1]);
  RETURN_VAL(OBJ_VAL(wrenConstruct<ObjRange>(vm, 0, from, to, true)));
}

DEF_PRIMITIVE(NumDotDotDot)
{
  if (!validateNum(vm, args[1], "Right hand side of range")) return false;

  double from = AS_NUM(args[0]);
  double to = AS_NUM(args[1]);
  RETURN_VAL(OBJ_VAL(wrenConstruct<ObjRange>(vm, 0, from, to, false)));
}

DEF_PRIMITIVE(NumAtan2)
{
  if (!validateNum(vm, args[1], "x value")) return false;

  RETURN_NUM(atan2(AS_NUM(args[0]), AS_NUM(args[1])));
}

DEF_PRIMITIVE(NumMin)
{
  if (!validateNum(vm, args[1], "Other value")) return false;

  double value = AS_NUM(args[0]);
  double other = AS_NUM(args[1]);
  RETURN_NUM(value <= other ? value : other);
}

DEF_PRIMITIVE(NumMax)
{
  if (!validateNum(vm, args[1], "Other value")) return false;

  double value = AS_NUM(args[0]);
  double other = AS_NUM(args[1]);
  RETURN_NUM(value > other ? value : other);
}

DEF_PRIMITIVE(NumClamp)
{
  if (!validateNum(vm, args[1], "Min value")) return false;
  if (!validateNum(vm, args[2], "Max value")) return false;

  double value = AS_NUM(args[0]);
  double min = AS_NUM(args[1]);
  double max = AS_NUM(args[2]);
  double result = (value < min) ? min : ((value > max) ? max : value);
  RETURN_NUM(result);
}

DEF_PRIMITIVE(NumPow)
{
  if (!validateNum(vm, args[1], "Power value")) return false;

  RETURN_NUM(pow(AS_NUM(args[0]), AS_NUM(args[1])));
}

DEF_PRIMITIVE(NumFraction)
{
  double unused;
  RETURN_NUM(modf(AS_NUM(args[0]) , &unused));
}

DEF_PRIMITIVE(NumIsInfinity)
{
  RETURN_BOOL(isinf(AS_NUM(args[0])));
}

DEF_PRIMITIVE(NumIsInteger)
{
  double value = AS_NUM(args[0]);
  if (isnan(value) || isinf(value)) RETURN_FALSE;
  RETURN_BOOL(trunc(value) == value);
}

DEF_PRIMITIVE(NumIsNan)
{
  RETURN_BOOL(isnan(AS_NUM(args[0])));
}

DEF_PRIMITIVE(NumSign)
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

DEF_PRIMITIVE(NumToString)
{
  RETURN_VAL(vm->NumToString( AS_NUM(args[0])));
}

DEF_PRIMITIVE(NumTruncate)
{
  double integer;
  modf(AS_NUM(args[0]) , &integer);
  RETURN_NUM(integer);
}

DEF_PRIMITIVE(ObjectSame)
{
  RETURN_BOOL(wrenValuesEqual(args[1], args[2]));
}

DEF_PRIMITIVE(ObjectNot)
{
  RETURN_VAL(FALSE_VAL);
}

DEF_PRIMITIVE(ObjectEqeq)
{
  RETURN_BOOL(wrenValuesEqual(args[0], args[1]));
}

DEF_PRIMITIVE(ObjectBangeq)
{
  RETURN_BOOL(!wrenValuesEqual(args[0], args[1]));
}

DEF_PRIMITIVE(ObjectIs)
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

DEF_PRIMITIVE(ObjectToString)
{
  Obj* obj = AS_OBJ(args[0]);
  Value name = OBJ_VAL(obj->classObj->name);
  RETURN_VAL(vm->StringFormat( "instance of @", name));
}

DEF_PRIMITIVE(ObjectType)
{
  RETURN_OBJ(vm->GetClass( args[0]));
}

DEF_PRIMITIVE(RangeFrom)
{
  RETURN_NUM(AS_RANGE(args[0])->from);
}

DEF_PRIMITIVE(RangeTo)
{
  RETURN_NUM(AS_RANGE(args[0])->to);
}

DEF_PRIMITIVE(RangeMin)
{
  ObjRange* range = AS_RANGE(args[0]);
  RETURN_NUM(fmin(range->from, range->to));
}

DEF_PRIMITIVE(RangeMax)
{
  ObjRange* range = AS_RANGE(args[0]);
  RETURN_NUM(fmax(range->from, range->to));
}

DEF_PRIMITIVE(RangeIsInclusive)
{
  RETURN_BOOL(AS_RANGE(args[0])->isInclusive);
}

DEF_PRIMITIVE(RangeIterate)
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

DEF_PRIMITIVE(RangeIteratorValue)
{
  // Assume the iterator is a number so that is the value of the range.
  RETURN_VAL(args[1]);
}

DEF_PRIMITIVE(RangeToString)
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

DEF_PRIMITIVE(StringFromCodePoint)
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

DEF_PRIMITIVE(StringFromByte)
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

DEF_PRIMITIVE(StringByteAt)
{
  ObjString* string = AS_STRING(args[0]);

  uint32_t index = validateIndex(vm, args[1], string->length, "Index");
  if (index == UINT32_MAX) return false;

  RETURN_NUM((uint8_t)string->value[index]);
}

DEF_PRIMITIVE(StringByteCount)
{
  RETURN_NUM(AS_STRING(args[0])->length);
}

DEF_PRIMITIVE(StringCodePointAt)
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

DEF_PRIMITIVE(StringContains)
{
  if (!validateString(vm, args[1], "Argument")) return false;

  ObjString* string = AS_STRING(args[0]);
  ObjString* search = AS_STRING(args[1]);

  RETURN_BOOL(wrenStringFind(string, search, 0) != UINT32_MAX);
}

DEF_PRIMITIVE(StringEndsWith)
{
  if (!validateString(vm, args[1], "Argument")) return false;

  ObjString* string = AS_STRING(args[0]);
  ObjString* search = AS_STRING(args[1]);

  // Edge case: If the search string is longer then return false right away.
  if (search->length > string->length) RETURN_FALSE;

  RETURN_BOOL(memcmp(string->value + string->length - search->length,
                     search->value, search->length) == 0);
}

DEF_PRIMITIVE(StringIndexOf1)
{
  if (!validateString(vm, args[1], "Argument")) return false;

  ObjString* string = AS_STRING(args[0]);
  ObjString* search = AS_STRING(args[1]);

  uint32_t index = wrenStringFind(string, search, 0);
  RETURN_NUM(index == UINT32_MAX ? -1 : (int)index);
}

DEF_PRIMITIVE(StringIndexOf2)
{
  if (!validateString(vm, args[1], "Argument")) return false;

  ObjString* string = AS_STRING(args[0]);
  ObjString* search = AS_STRING(args[1]);
  uint32_t start = validateIndex(vm, args[2], string->length, "Start");
  if (start == UINT32_MAX) return false;
  
  uint32_t index = wrenStringFind(string, search, start);
  RETURN_NUM(index == UINT32_MAX ? -1 : (int)index);
}

DEF_PRIMITIVE(StringIterate)
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

DEF_PRIMITIVE(StringIterateByte)
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

DEF_PRIMITIVE(StringIteratorValue)
{
  ObjString* string = AS_STRING(args[0]);
  uint32_t index = validateIndex(vm, args[1], string->length, "Iterator");
  if (index == UINT32_MAX) return false;

  RETURN_VAL(vm->StringCodePointAt( string, index));
}

DEF_PRIMITIVE(StringStartsWith)
{
  if (!validateString(vm, args[1], "Argument")) return false;

  ObjString* string = AS_STRING(args[0]);
  ObjString* search = AS_STRING(args[1]);

  // Edge case: If the search string is longer then return false right away.
  if (search->length > string->length) RETURN_FALSE;

  RETURN_BOOL(memcmp(string->value, search->value, search->length) == 0);
}

DEF_PRIMITIVE(StringPlus)
{
  if (!validateString(vm, args[1], "Right operand")) return false;
  RETURN_VAL(vm->StringFormat( "@@", args[0], args[1]));
}

DEF_PRIMITIVE(StringSubscript)
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

DEF_PRIMITIVE(StringToString)
{
  RETURN_VAL(args[0]);
}

DEF_PRIMITIVE(SystemClock)
{
  RETURN_NUM((double)clock() / CLOCKS_PER_SEC);
}

DEF_PRIMITIVE(SystemGc)
{
  wrenCollectGarbage(vm);
  RETURN_NULL;
}

DEF_PRIMITIVE(SystemWriteString)
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
  PRIMITIVE(this->objectClass, "!", ObjectNot);
  PRIMITIVE(this->objectClass, "==(_)", ObjectEqeq);
  PRIMITIVE(this->objectClass, "!=(_)", ObjectBangeq);
  PRIMITIVE(this->objectClass, "is(_)", ObjectIs);
  PRIMITIVE(this->objectClass, "toString", ObjectToString);
  PRIMITIVE(this->objectClass, "type", ObjectType);

  // Now we can define Class, which is a subclass of Object.
  this->classClass = this->DefineClass(coreModule, "Class");
  this->BindSuperclass( this->classClass, this->objectClass);
  PRIMITIVE(this->classClass, "name", ClassName);
  PRIMITIVE(this->classClass, "supertype", ClassSupertype);
  PRIMITIVE(this->classClass, "toString", ClassToString);
  PRIMITIVE(this->classClass, "attributes", ClassAttributes);

  // Finally, we can define Object's metaclass which is a subclass of Class.
  ObjClass* objectMetaclass = this->DefineClass(coreModule, "Object metaclass");

  // Wire up the metaclass relationships now that all three classes are built.
  this->objectClass->classObj = objectMetaclass;
  objectMetaclass->classObj = this->classClass;
  this->classClass->classObj = this->classClass;

  // Do this after wiring up the metaclasses so objectMetaclass doesn't get
  // collected.
  this->BindSuperclass( objectMetaclass, this->classClass);

  PRIMITIVE(objectMetaclass, "same(_,_)", ObjectSame);

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
  PRIMITIVE(this->boolClass, "toString", BoolToString);
  PRIMITIVE(this->boolClass, "!", BoolNot);

  this->fiberClass = AS_CLASS(this->FindVariable( coreModule, "Fiber"));
  PRIMITIVE(this->fiberClass->classObj, "new(_)", FiberNew);
  PRIMITIVE(this->fiberClass->classObj, "abort(_)", FiberAbort);
  PRIMITIVE(this->fiberClass->classObj, "exit(_)", FiberExit);
  PRIMITIVE(this->fiberClass->classObj, "current", FiberCurrent);
  PRIMITIVE(this->fiberClass->classObj, "suspend()", FiberSuspend);
  PRIMITIVE(this->fiberClass->classObj, "yield()", FiberYield);
  PRIMITIVE(this->fiberClass->classObj, "yield(_)", FiberYield1);
  PRIMITIVE(this->fiberClass, "call()", FiberCall);
  PRIMITIVE(this->fiberClass, "call(_)", FiberCall1);
  PRIMITIVE(this->fiberClass, "error", FiberError);
  PRIMITIVE(this->fiberClass, "isDone", FiberIsDone);
  PRIMITIVE(this->fiberClass, "transfer()", FiberTransfer);
  PRIMITIVE(this->fiberClass, "transfer(_)", FiberTransfer1);
  PRIMITIVE(this->fiberClass, "transferError(_)", FiberTransferError);
  PRIMITIVE(this->fiberClass, "try()", FiberTry);
  PRIMITIVE(this->fiberClass, "try(_)", FiberTry1);

  this->fnClass = AS_CLASS(this->FindVariable( coreModule, "Fn"));
  PRIMITIVE(this->fnClass->classObj, "new(_)", FnNew);

  PRIMITIVE(this->fnClass, "arity", FnArity);

  FUNCTION_CALL(this->fnClass, "call()", FnCall0);
  FUNCTION_CALL(this->fnClass, "call(_)", FnCall1);
  FUNCTION_CALL(this->fnClass, "call(_,_)", FnCall2);
  FUNCTION_CALL(this->fnClass, "call(_,_,_)", FnCall3);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_)", FnCall4);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_)", FnCall5);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_)", FnCall6);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_)", FnCall7);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_)", FnCall8);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_)", FnCall9);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_)", FnCall10);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_)", FnCall11);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_,_)", FnCall12);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_,_,_)", FnCall13);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_)", FnCall14);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)", FnCall15);
  FUNCTION_CALL(this->fnClass, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)", FnCall16);
  
  PRIMITIVE(this->fnClass, "toString", FnToString);

  this->nullClass = AS_CLASS(this->FindVariable( coreModule, "Null"));
  PRIMITIVE(this->nullClass, "!", NullNot);
  PRIMITIVE(this->nullClass, "toString", NullToString);

  this->numClass = AS_CLASS(this->FindVariable( coreModule, "Num"));
  PRIMITIVE(this->numClass->classObj, "fromString(_)", NumFromString);
  PRIMITIVE(this->numClass->classObj, "infinity", NumInfinity);
  PRIMITIVE(this->numClass->classObj, "nan", NumNan);
  PRIMITIVE(this->numClass->classObj, "pi", NumPi);
  PRIMITIVE(this->numClass->classObj, "tau", NumTau);
  PRIMITIVE(this->numClass->classObj, "largest", NumLargest);
  PRIMITIVE(this->numClass->classObj, "smallest", NumSmallest);
  PRIMITIVE(this->numClass->classObj, "maxSafeInteger", NumMaxSafeInteger);
  PRIMITIVE(this->numClass->classObj, "minSafeInteger", NumMinSafeInteger);
  PRIMITIVE(this->numClass, "-(_)", NumMinus);
  PRIMITIVE(this->numClass, "+(_)", NumPlus);
  PRIMITIVE(this->numClass, "*(_)", NumMultiply);
  PRIMITIVE(this->numClass, "/(_)", NumDivide);
  PRIMITIVE(this->numClass, "<(_)", NumLt);
  PRIMITIVE(this->numClass, ">(_)", NumGt);
  PRIMITIVE(this->numClass, "<=(_)", NumLte);
  PRIMITIVE(this->numClass, ">=(_)", NumGte);
  PRIMITIVE(this->numClass, "&(_)", NumBitwiseAnd);
  PRIMITIVE(this->numClass, "|(_)", NumBitwiseOr);
  PRIMITIVE(this->numClass, "^(_)", NumBitwiseXor);
  PRIMITIVE(this->numClass, "<<(_)", NumBitwiseLeftShift);
  PRIMITIVE(this->numClass, ">>(_)", NumBitwiseRightShift);
  PRIMITIVE(this->numClass, "abs", NumAbs);
  PRIMITIVE(this->numClass, "acos", NumAcos);
  PRIMITIVE(this->numClass, "asin", NumAsin);
  PRIMITIVE(this->numClass, "atan", NumAtan);
  PRIMITIVE(this->numClass, "cbrt", NumCbrt);
  PRIMITIVE(this->numClass, "ceil", NumCeil);
  PRIMITIVE(this->numClass, "cos", NumCos);
  PRIMITIVE(this->numClass, "floor", NumFloor);
  PRIMITIVE(this->numClass, "-", NumNegate);
  PRIMITIVE(this->numClass, "round", NumRound);
  PRIMITIVE(this->numClass, "min(_)", NumMin);
  PRIMITIVE(this->numClass, "max(_)", NumMax);
  PRIMITIVE(this->numClass, "clamp(_,_)", NumClamp);
  PRIMITIVE(this->numClass, "sin", NumSin);
  PRIMITIVE(this->numClass, "sqrt", NumSqrt);
  PRIMITIVE(this->numClass, "tan", NumTan);
  PRIMITIVE(this->numClass, "log", NumLog);
  PRIMITIVE(this->numClass, "log2", NumLog2);
  PRIMITIVE(this->numClass, "exp", NumExp);
  PRIMITIVE(this->numClass, "%(_)", NumMod);
  PRIMITIVE(this->numClass, "~", NumBitwiseNot);
  PRIMITIVE(this->numClass, "..(_)", NumDotDot);
  PRIMITIVE(this->numClass, "...(_)", NumDotDotDot);
  PRIMITIVE(this->numClass, "atan(_)", NumAtan2);
  PRIMITIVE(this->numClass, "pow(_)", NumPow);
  PRIMITIVE(this->numClass, "fraction", NumFraction);
  PRIMITIVE(this->numClass, "isInfinity", NumIsInfinity);
  PRIMITIVE(this->numClass, "isInteger", NumIsInteger);
  PRIMITIVE(this->numClass, "isNan", NumIsNan);
  PRIMITIVE(this->numClass, "sign", NumSign);
  PRIMITIVE(this->numClass, "toString", NumToString);
  PRIMITIVE(this->numClass, "truncate", NumTruncate);

  // These are defined just so that 0 and -0 are equal, which is specified by
  // IEEE 754 even though they have different bit representations.
  PRIMITIVE(this->numClass, "==(_)", NumEqeq);
  PRIMITIVE(this->numClass, "!=(_)", NumBangeq);

  this->stringClass = AS_CLASS(this->FindVariable( coreModule, "String"));
  PRIMITIVE(this->stringClass->classObj, "fromCodePoint(_)", StringFromCodePoint);
  PRIMITIVE(this->stringClass->classObj, "fromByte(_)", StringFromByte);
  PRIMITIVE(this->stringClass, "+(_)", StringPlus);
  PRIMITIVE(this->stringClass, "[_]", StringSubscript);
  PRIMITIVE(this->stringClass, "byteAt_(_)", StringByteAt);
  PRIMITIVE(this->stringClass, "byteCount_", StringByteCount);
  PRIMITIVE(this->stringClass, "codePointAt_(_)", StringCodePointAt);
  PRIMITIVE(this->stringClass, "contains(_)", StringContains);
  PRIMITIVE(this->stringClass, "endsWith(_)", StringEndsWith);
  PRIMITIVE(this->stringClass, "indexOf(_)", StringIndexOf1);
  PRIMITIVE(this->stringClass, "indexOf(_,_)", StringIndexOf2);
  PRIMITIVE(this->stringClass, "iterate(_)", StringIterate);
  PRIMITIVE(this->stringClass, "iterateByte_(_)", StringIterateByte);
  PRIMITIVE(this->stringClass, "iteratorValue(_)", StringIteratorValue);
  PRIMITIVE(this->stringClass, "startsWith(_)", StringStartsWith);
  PRIMITIVE(this->stringClass, "toString", StringToString);

  this->listClass = AS_CLASS(this->FindVariable( coreModule, "List"));
  PRIMITIVE(this->listClass->classObj, "filled(_,_)", ListFilled);
  PRIMITIVE(this->listClass->classObj, "new()", ListNew);
  PRIMITIVE(this->listClass, "[_]", ListSubscript);
  PRIMITIVE(this->listClass, "[_]=(_)", ListSubscriptSetter);
  PRIMITIVE(this->listClass, "add(_)", ListAdd);
  PRIMITIVE(this->listClass, "addCore_(_)", ListAddCore);
  PRIMITIVE(this->listClass, "clear()", ListClear);
  PRIMITIVE(this->listClass, "count", ListCount);
  PRIMITIVE(this->listClass, "insert(_,_)", ListInsert);
  PRIMITIVE(this->listClass, "iterate(_)", ListIterate);
  PRIMITIVE(this->listClass, "iteratorValue(_)", ListIteratorValue);
  PRIMITIVE(this->listClass, "removeAt(_)", ListRemoveAt);
  PRIMITIVE(this->listClass, "swap(_,_)", ListSwap);

  this->mapClass = AS_CLASS(this->FindVariable( coreModule, "Map"));
  PRIMITIVE(this->mapClass->classObj, "new()", MapNew);
  PRIMITIVE(this->mapClass, "[_]", MapSubscript);
  PRIMITIVE(this->mapClass, "[_]=(_)", MapSubscriptSetter);
  PRIMITIVE(this->mapClass, "addCore_(_,_)", MapAddCore);
  PRIMITIVE(this->mapClass, "clear()", MapClear);
  PRIMITIVE(this->mapClass, "containsKey(_)", MapContainsKey);
  PRIMITIVE(this->mapClass, "count", MapCount);
  PRIMITIVE(this->mapClass, "remove(_)", MapRemove);
  PRIMITIVE(this->mapClass, "iterate(_)", MapIterate);
  PRIMITIVE(this->mapClass, "keyIteratorValue_(_)", MapKeyIteratorValue);
  PRIMITIVE(this->mapClass, "valueIteratorValue_(_)", MapValueIteratorValue);

  this->rangeClass = AS_CLASS(this->FindVariable( coreModule, "Range"));
  PRIMITIVE(this->rangeClass, "from", RangeFrom);
  PRIMITIVE(this->rangeClass, "to", RangeTo);
  PRIMITIVE(this->rangeClass, "min", RangeMin);
  PRIMITIVE(this->rangeClass, "max", RangeMax);
  PRIMITIVE(this->rangeClass, "isInclusive", RangeIsInclusive);
  PRIMITIVE(this->rangeClass, "iterate(_)", RangeIterate);
  PRIMITIVE(this->rangeClass, "iteratorValue(_)", RangeIteratorValue);
  PRIMITIVE(this->rangeClass, "toString", RangeToString);

  ObjClass* systemClass = AS_CLASS(this->FindVariable( coreModule, "System"));
  PRIMITIVE(systemClass->classObj, "clock", SystemClock);
  PRIMITIVE(systemClass->classObj, "gc()", SystemGc);
  PRIMITIVE(systemClass->classObj, "writeString_(_)", SystemWriteString);

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
