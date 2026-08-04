// Standalone test for C++20 foreign bindings API
// This tests the modern C++20 API for foreign class bindings (wren.hpp)

#include <cstdio>
#include <cmath>
#include <string>
#include "../src/include/wren.hpp"

// Test counter
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
  do { \
    if (condition) { \
      testsPassed++; \
      printf("PASS: %s\n", message); \
    } else { \
      testsFailed++; \
      fprintf(stderr, "FAIL: %s\n", message); \
    } \
  } while(0)

// Test classes for C++ bindings

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

class Calculator {
public:
  Calculator() : value(0) {}
  explicit Calculator(double initialValue) : value(initialValue) {}
  
  double add(double a, double b) { return a + b; }
  double multiply(double a, double b) { return a * b; }
  double getResult() const { return value; }
  void setResult(double v) { value = v; }
  
  static double pi() { return 3.14159; }
  
private:
  double value;
};

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

int main() {
  printf("Testing C++20 Foreign Bindings API\n");
  printf("=====================================\n\n");
  
  // Configure VM with write function
  wren::VM::Config config;
  config.wrenConfig.writeFn = [](WrenVM*, const char* text) { printf("%s", text); };
  config.wrenConfig.errorFn = [](WrenVM*, WrenErrorType, const char* mod, int line, const char* msg) {
    fprintf(stderr, "[%s:%d] %s\n", mod ? mod : "?", line, msg);
  };
  
  wren::VM vm(std::move(config));
  
  // Test 1: Basic foreign class with default constructor
  {
    printf("Test 1: Basic foreign class with default constructor\n");
    auto& math = vm.module("test_basic");
    auto point = math.klass<Point>("Point");
    point.ctor<>();
    point.func<&Point::length>("length");
    point.func<&Point::getX>("getX");
    point.func<&Point::setX, double>("setX");
  }
  
  const char* test1Source = R"WREN(
import "test_basic" for Point

var p = Point.new()
System.print("default getX: %(p.getX)")
System.print("default length: %(p.length)")

p.setX(3.0)
System.print("after setX: %(p.getX)")
System.print("length after setX: %(p.length)")
)WREN";
  
  auto result = vm.interpret("test1", test1Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Basic foreign class test");
  printf("\n");
  
  // Test 2: Foreign class with parameterized constructor
  {
    printf("Test 2: Foreign class with parameterized constructor\n");
    auto& math = vm.module("test_constructor");
    auto point = math.klass<Point>("Point");
    point.ctor<>();
    point.ctor<double, double, double>();
    point.func<&Point::length>("length");
  }
  
  const char* test2Source = R"WREN(
import "test_constructor" for Point

var p1 = Point.new()
System.print("default length: %(p1.length)")

var p2 = Point.new(3.0, 4.0, 0.0)
System.print("3-4-0 length: %(p2.length)")
)WREN";
  
  result = vm.interpret("test2", test2Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Parameterized constructor test");
  printf("\n");
  
  // Test 3: Static methods
  {
    printf("Test 3: Static methods\n");
    auto& math = vm.module("test_static");
    auto point = math.klass<Point>("Point");
    point.ctor<>();
    point.funcStatic<&Point::origin>("origin");
    point.func<&Point::length>("length");
  }
  
  const char* test3Source = R"WREN(
import "test_static" for Point

var p = Point.origin()
System.print("origin length: %(p.length)")
)WREN";
  
  result = vm.interpret("test3", test3Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Static methods test");
  printf("\n");
  
  // Test 4: Properties with getters and setters
  {
    printf("Test 4: Properties with getters and setters\n");
    auto& math = vm.module("test_properties");
    auto point = math.klass<Point>("Point");
    point.ctor<>();
    point.prop<&Point::getX, &Point::setX, double>("x");
  }
  
  const char* test4Source = R"WREN(
import "test_properties" for Point

var p = Point.new()
System.print("initial x: %(p.x)")

p.x = 5.0
System.print("set x: %(p.x)")
)WREN";
  
  result = vm.interpret("test4", test4Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Properties with getters and setters test");
  printf("\n");
  
  // Test 5: Read-only properties
  {
    printf("Test 5: Read-only properties\n");
    auto& math = vm.module("test_readonly");
    auto point = math.klass<Point>("Point");
    point.ctor<double, double, double>();
    point.propReadonly<&Point::length>("length");
  }
  
  const char* test5Source = R"WREN(
import "test_readonly" for Point

var p = Point.new(3.0, 4.0, 0.0)
System.print("readonly length: %(p.length)")
)WREN";
  
  result = vm.interpret("test5", test5Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Read-only properties test");
  printf("\n");
  
  // Test 6: String handling
  {
    printf("Test 6: String handling\n");
    auto& social = vm.module("test_strings");
    auto person = social.klass<Person>("Person");
    person.ctor<>();
    person.ctor<std::string, int>();
    person.func<&Person::introduce>("introduce");
    person.func<&Person::isAdult>("isAdult");
  }
  
  const char* test6Source = R"WREN(
import "test_strings" for Person

var p = Person.new("Alice", 25)
System.print(p.introduce())
System.print("is adult: %(p.isAdult())")
)WREN";
  
  result = vm.interpret("test6", test6Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "String handling test");
  printf("\n");
  
  // Test 7: Finalizers
  TrackedResource::finalizeCount = 0;
  
  {
    printf("Test 7: Finalizers\n");
    auto& resMod = vm.module("test_finalizer");
    auto resource = resMod.klass<TrackedResource>("Resource");
    resource.ctor<>();
    resource.ctor<int>();
    resource.func<&TrackedResource::getId>("getId");
    resource.finalize();
  }
  
  const char* test7Source = R"WREN(
import "test_finalizer" for Resource

var r1 = Resource.new(1)
System.print("resource1 id: %(r1.getId())")

var r2 = Resource.new(2)
System.print("resource2 id: %(r2.getId())")
)WREN";
  
  result = vm.interpret("test7", test7Source);
  
  // Force garbage collection to trigger finalizers
  vm.collectGarbage();
  vm.collectGarbage();
  
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Finalizers execution test");
  TEST_ASSERT(TrackedResource::finalizeCount >= 2, "Finalizers called test");
  printf("\n");
  
  // Test 8: Multiple constructors
  {
    printf("Test 8: Multiple constructors\n");
    auto& calcMod = vm.module("test_multiple_ctor");
    auto calculator = calcMod.klass<Calculator>("Calculator");
    calculator.ctor<>();
    calculator.ctor<double>();
    calculator.func<&Calculator::add, double, double>("add");
    calculator.func<&Calculator::multiply, double, double>("multiply");
    calculator.func<&Calculator::getResult>("getResult");
  }
  
  const char* test8Source = R"WREN(
import "test_multiple_ctor" for Calculator

var c1 = Calculator.new()
System.print("default result: %(c1.getResult())")

var c2 = Calculator.new(42.0)
System.print("custom result: %(c2.getResult())")

System.print("add: %(c2.add(3, 4))")
System.print("multiply: %(c2.multiply(3, 4))")
)WREN";
  
  result = vm.interpret("test8", test8Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Multiple constructors test");
  printf("\n");
  
  // Test 9: Static methods
  {
    printf("Test 9: Static methods\n");
    auto& calcMod2 = vm.module("test_static_methods");
    auto calculator2 = calcMod2.klass<Calculator>("Calculator");
    calculator2.ctor<>();
    calculator2.funcStatic<&Calculator::pi>("pi");
  }
  
  const char* test9Source = R"WREN(
import "test_static_methods" for Calculator

System.print("pi: %(Calculator.pi())")
)WREN";
  
  result = vm.interpret("test9", test9Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Static methods test");
  printf("\n");
  
  // Summary
  printf("=====================================\n");
  printf("C++20 Foreign Binding Tests Complete\n");
  printf("Passed: %d, Failed: %d\n", testsPassed, testsFailed);
  
  if (testsFailed == 0) {
    printf("\n✓ All tests passed!\n");
    return 0;
  } else {
    printf("\n✗ Some tests failed!\n");
    return 1;
  }
}