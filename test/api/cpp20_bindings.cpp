#include <cmath>
#include <cstring>
#include <string>
#include "cpp20_bindings.h"

// Test counter for verification
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
  do { \
    if (condition) { \
      testsPassed++; \
    } else { \
      fprintf(stderr, "FAILED: %s\n", message); \
      testsFailed++; \
    } \
  } while(0)

// Test classes for C++ bindings

// Simple class with basic functionality
class Point {
public:
  double x, y, z;
  
  Point() : x(0), y(0), z(0) {}
  Point(double x, double y, double z) : x(x), y(y), z(z) {}
  
  double length() const {
    return sqrt(x*x + y*y + z*z);
  }
  
  double getX() const { return x; }
  void setX(double v) { x = v; }
  
  static Point origin() { return Point(0, 0, 0); }
};

// Class with string handling
class Person {
public:
  std::string name;
  int age;
  
  Person() : name(""), age(0) {}
  Person(const std::string& name, int age) : name(name), age(age) {}
  
  std::string introduce() const {
    return "I am " + name + ", age " + std::to_string(age);
  }
  
  bool isAdult() const { return age >= 18; }
};

// Class with finalization tracking
class TrackedResource {
public:
  static int finalizeCount;
  int id;
  
  TrackedResource() : id(0) {}
  explicit TrackedResource(int id) : id(id) {}
  
  ~TrackedResource() {
    finalizeCount++;
  }
  
  int getId() const { return id; }
};

int TrackedResource::finalizeCount = 0;

// Class with multiple constructors
class Calculator {
public:
  Calculator() {}
  Calculator(double initialValue) : value(initialValue) {}
  
  double add(double a, double b) { return a + b; }
  double multiply(double a, double b) { return a * b; }
  double getResult() const { return value; }
  void setResult(double v) { value = v; }
  
  static double pi() { return 3.14159; }
  
private:
  double value = 0;
};

// Foreign method binding functions using C API (for compatibility with test framework)

static void pointAllocate(WrenVM* vm)
{
  double* coords = (double*)wrenSetSlotNewForeign(vm, 0, 0, sizeof(double[3]));
  
  if (wrenGetSlotCount(vm) == 1) {
    coords[0] = 0.0;
    coords[1] = 0.0;
    coords[2] = 0.0;
  } else {
    coords[0] = wrenGetSlotDouble(vm, 1);
    coords[1] = wrenGetSlotDouble(vm, 2);
    coords[2] = wrenGetSlotDouble(vm, 3);
  }
}

static void pointLength(WrenVM* vm)
{
  double* coords = (double*)wrenGetSlotForeign(vm, 0);
  double len = sqrt(coords[0]*coords[0] + coords[1]*coords[1] + coords[2]*coords[2]);
  wrenSetSlotDouble(vm, 0, len);
}

static void pointGetX(WrenVM* vm)
{
  double* coords = (double*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, coords[0]);
}

static void pointSetX(WrenVM* vm)
{
  double* coords = (double*)wrenGetSlotForeign(vm, 0);
  coords[0] = wrenGetSlotDouble(vm, 1);
}

static void pointOrigin(WrenVM* vm)
{
  double* coords = (double*)wrenSetSlotNewForeign(vm, 0, 0, sizeof(double[3]));
  coords[0] = 0.0;
  coords[1] = 0.0;
  coords[2] = 0.0;
}

static void personAllocate(WrenVM* vm)
{
  Person* person = (Person*)wrenSetSlotNewForeign(vm, 0, 0, sizeof(Person));
  
  if (wrenGetSlotCount(vm) == 1) {
    new(person) Person();
  } else {
    const char* name = wrenGetSlotString(vm, 1);
    int age = (int)wrenGetSlotDouble(vm, 2);
    new(person) Person(name, age);
  }
}

static void personFinalize(void* data)
{
  Person* person = (Person*)data;
  person->~Person();
}

static void personIntroduce(WrenVM* vm)
{
  Person* person = (Person*)wrenGetSlotForeign(vm, 0);
  std::string intro = person->introduce();
  wrenSetSlotString(vm, 0, intro.c_str());
}

static void personIsAdult(WrenVM* vm)
{
  Person* person = (Person*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotBool(vm, 0, person->isAdult());
}

static void resourceAllocate(WrenVM* vm)
{
  TrackedResource* resource = (TrackedResource*)wrenSetSlotNewForeign(vm, 0, 0, sizeof(TrackedResource));
  
  if (wrenGetSlotCount(vm) == 1) {
    new(resource) TrackedResource();
  } else {
    int id = (int)wrenGetSlotDouble(vm, 1);
    new(resource) TrackedResource(id);
  }
}

static void resourceFinalize(void* data)
{
  TrackedResource* resource = (TrackedResource*)data;
  resource->~TrackedResource();
}

static void resourceGetId(WrenVM* vm)
{
  TrackedResource* resource = (TrackedResource*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, resource->getId());
}

static void calculatorAllocate(WrenVM* vm)
{
  Calculator* calc = (Calculator*)wrenSetSlotNewForeign(vm, 0, 0, sizeof(Calculator));
  
  if (wrenGetSlotCount(vm) == 1) {
    new(calc) Calculator();
  } else {
    double initialValue = wrenGetSlotDouble(vm, 1);
    new(calc) Calculator(initialValue);
  }
}

static void calculatorAdd(WrenVM* vm)
{
  Calculator* calc = (Calculator*)wrenGetSlotForeign(vm, 0);
  double a = wrenGetSlotDouble(vm, 1);
  double b = wrenGetSlotDouble(vm, 2);
  wrenSetSlotDouble(vm, 0, calc->add(a, b));
}

static void calculatorMultiply(WrenVM* vm)
{
  Calculator* calc = (Calculator*)wrenGetSlotForeign(vm, 0);
  double a = wrenGetSlotDouble(vm, 1);
  double b = wrenGetSlotDouble(vm, 2);
  wrenSetSlotDouble(vm, 0, calc->multiply(a, b));
}

static void calculatorGetResult(WrenVM* vm)
{
  Calculator* calc = (Calculator*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, calc->getResult());
}

static void calculatorPi(WrenVM* vm)
{
  wrenSetSlotDouble(vm, 0, Calculator::pi());
}

// Foreign method binding
WrenForeignMethodFn cpp20BindingsBindMethod(const char* signature)
{
  if (strcmp(signature, "static Point.origin") == 0) return pointOrigin;
  if (strcmp(signature, "Point.length") == 0) return pointLength;
  if (strcmp(signature, "Point.getX") == 0) return pointGetX;
  if (strcmp(signature, "Point.setX(_)") == 0) return pointSetX;
  
  if (strcmp(signature, "Person.introduce") == 0) return personIntroduce;
  if (strcmp(signature, "Person.isAdult") == 0) return personIsAdult;
  
  if (strcmp(signature, "Resource.getId") == 0) return resourceGetId;
  
  if (strcmp(signature, "Calculator.add(_,_)") == 0) return calculatorAdd;
  if (strcmp(signature, "Calculator.multiply(_,_)") == 0) return calculatorMultiply;
  if (strcmp(signature, "Calculator.getResult") == 0) return calculatorGetResult;
  if (strcmp(signature, "static Calculator.pi") == 0) return calculatorPi;
  
  return NULL;
}

// Foreign class binding
void cpp20BindingsBindClass(const char* className, WrenForeignClassMethods* methods)
{
  if (strcmp(className, "Point") == 0) {
    methods->allocate = pointAllocate;
    return;
  }
  
  if (strcmp(className, "Person") == 0) {
    methods->allocate = personAllocate;
    methods->finalize = personFinalize;
    return;
  }
  
  if (strcmp(className, "Resource") == 0) {
    methods->allocate = resourceAllocate;
    methods->finalize = resourceFinalize;
    return;
  }
  
  if (strcmp(className, "Calculator") == 0) {
    methods->allocate = calculatorAllocate;
    return;
  }
}

// Test runner implementation
int cpp20BindingsRunTests(WrenVM* vm)
{
  // Reset counters
  testsPassed = 0;
  testsFailed = 0;
  
  printf("Running Foreign Binding Tests...\n");
  
  // Test 1: Basic foreign class with default constructor
  const char* test1Source = R"WREN(
foreign class Point {
  construct new() {}
  foreign length
  foreign getX
  foreign setX(_)
}

var p = Point.new()
System.print("default getX: %(p.getX)") // expect: 0
System.print("default length: %(p.length)") // expect: 0

p.setX(3.0)
System.print("after setX: %(p.getX)") // expect: 3
System.print("length after setX: %(p.length)") // expect: 3
)WREN";
  
  auto result = wrenInterpret(vm, "test1", test1Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Test 1: Basic foreign class");
  
  // Test 2: Foreign class with parameterized constructor
  const char* test2Source = R"WREN(
foreign class Point {
  construct new() {}
  construct new(_,_,_) {}
  foreign length
}

var p1 = Point.new()
System.print("default length: %(p1.length)") // expect: 0

var p2 = Point.new(3.0, 4.0, 0.0)
System.print("3-4-0 length: %(p2.length)") // expect: 5
)WREN";
  
  result = wrenInterpret(vm, "test2", test2Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Test 2: Parameterized constructor");
  
  // Test 3: Static methods
  const char* test3Source = R"WREN(
foreign class Point {
  construct new(_,_,_) {}
  foreign static origin
  foreign length
}

var p = Point.origin()
System.print("origin length: %(p.length)") // expect: 0
)WREN";
  
  result = wrenInterpret(vm, "test3", test3Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Test 3: Static methods");
  
  // Test 4: String handling with finalizers
  TrackedResource::finalizeCount = 0;
  
  const char* test4Source = R"WREN(
foreign class Person {
  construct new() {}
  construct new(_,_) {}
  foreign introduce
  foreign isAdult
}

var p = Person.new("Alice", 25)
System.print(p.introduce()) // expect: I am Alice, age 25
System.print("is adult: %(p.isAdult())") // expect: true
)WREN";
  
  result = wrenInterpret(vm, "test4", test4Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Test 4: String handling");
  
  // Test 5: Finalizers
  const char* test5Source = R"WREN(
foreign class Resource {
  construct new() {}
  construct new(_) {}
  foreign getId
}

var r1 = Resource.new(1)
System.print("resource1 id: %(r1.getId())") // expect: 1

var r2 = Resource.new(2)
System.print("resource2 id: %(r2.getId())") // expect: 2
)WREN";
  
  result = wrenInterpret(vm, "test5", test5Source);
  
  // Force garbage collection to trigger finalizers
  wrenCollectGarbage(vm);
  wrenCollectGarbage(vm);
  
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Test 5: Finalizers execution");
  TEST_ASSERT(TrackedResource::finalizeCount >= 2, "Test 5: Finalizers called");
  
  // Test 6: Multiple constructors
  const char* test6Source = R"WREN(
foreign class Calculator {
  construct new() {}
  construct new(_) {}
  foreign add(_,_)
  foreign multiply(_,_)
  foreign getResult
  foreign static pi
}

var c1 = Calculator.new()
System.print("default result: %(c1.getResult())") // expect: 0

var c2 = Calculator.new(42.0)
System.print("custom result: %(c2.getResult())") // expect: 42

System.print("add: %(c2.add(3, 4))") // expect: 7
System.print("multiply: %(c2.multiply(3, 4))") // expect: 12
System.print("pi: %(Calculator.pi())") // expect: 3.14159
)WREN";
  
  result = wrenInterpret(vm, "test6", test6Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Test 6: Multiple constructors and static methods");
  
  printf("Foreign Binding Tests Complete\n");
  printf("Passed: %d, Failed: %d\n", testsPassed, testsFailed);
  
  return testsFailed;
}
