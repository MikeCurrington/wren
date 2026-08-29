// Standalone test for C++20 foreign bindings API
// This tests the modern C++20 API for foreign class bindings (wren.hpp)

#include <cstdio>
#include <cmath>
#include <string>
#include "../src/include/wren.hpp"

#ifdef WREN_ENABLE_DEBUGGER
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

class PointDouble {
public:
  double x, y, z;
  
  PointDouble() : x(0), y(0), z(0) {}
  PointDouble(double x, double y, double z) : x(x), y(y), z(z) {}
  
  double length() const {
    return sqrt(x*x + y*y + z*z);
  }
  
  double getX() const { return x; }
  void setX(double v) { x = v; }

  // Takes another PointDouble as a foreign parameter
  double distanceTo(const PointDouble& other) const {
    double dx = x - other.x;
    double dy = y - other.y;
    double dz = z - other.z;
    return sqrt(dx*dx + dy*dy + dz*dz);
  }

  static PointDouble origin() { return PointDouble(0, 0, 0); }
};

class PointFloat {
public:
  float x, y, z;
  
  PointFloat() : x(0), y(0), z(0) {}
  PointFloat(float x, float y, float z) : x(x), y(y), z(z) {}
  
  float length() const {
    return sqrtf(x*x + y*y + z*z);
  }
  
  float getX() const { return x; }
  void setX(float v) { x = v; }

  // Takes another PointFloat as a foreign parameter
  float distanceTo(const PointFloat& other) const {
    float dx = x - other.x;
    float dy = y - other.y;
    float dz = z - other.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
  }

  static PointFloat origin() { return PointFloat(0, 0, 0); }
};

class PointInt {
public:
  int x, y, z;
  
  PointInt() : x(0), y(0), z(0) {}
  PointInt(int x, int y, int z) : x(x), y(y), z(z) {}
  int getX() const { return x; }
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

// Test classes for foreign / shared_ptr return values
class Widget {
public:
  int counter = 0;
  std::string tag = "widget";
  std::shared_ptr<Widget> child;

  static std::shared_ptr<Widget> root;

  void bump(int by) { counter += by; }
  std::shared_ptr<Widget> getChild() { return child; }
  Widget copy() const { Widget w; w.counter = counter; w.tag = tag; return w; }
  static std::shared_ptr<Widget> getRoot() { return root; }
  static std::shared_ptr<Widget> make(int start) {
    auto w = std::make_shared<Widget>();
    w->counter = start;
    return w;
  }
};

std::shared_ptr<Widget> Widget::root;

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
    auto point = math.klass<PointDouble>("Point");
    point.ctor<>();
    point.func<&PointDouble::length>("length");
    point.func<&PointDouble::getX>("getX");
    point.func<&PointDouble::setX, double>("setX");
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
    auto point = math.klass<PointDouble>("Point");
    point.ctor<>();
    point.ctor<double, double, double>();
    point.func<&PointDouble::length>("length");
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
    auto point = math.klass<PointDouble>("Point");
    point.ctor<>();
    point.funcStatic<&PointDouble::origin>("origin");
    point.func<&PointDouble::length>("length");
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
    auto point = math.klass<PointDouble>("Point");
    point.ctor<>();
    point.prop<&PointDouble::getX, &PointDouble::setX>("x");
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
    auto point = math.klass<PointDouble>("Point");
    point.ctor<double, double, double>();
    point.propReadonly<&PointDouble::length>("length");
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

{
  var r1 = Resource.new(1)
  System.print("resource1 id: %(r1.getId())")

  var r2 = Resource.new(2)
  System.print("resource2 id: %(r2.getId())")
}
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
  
  // Test 10: Passing foreign types as parameters
  {
    printf("Test 10: Passing foreign types as parameters\n");
    auto& math = vm.module("test_foreign_params");
    auto point = math.klass<PointDouble>("Point");
    point.ctor<>();
    point.ctor<double, double, double>();
    point.func<&PointDouble::length>("length");
    point.func<&PointDouble::distanceTo, PointDouble>("distanceTo");
  }

  const char* test10Source = R"WREN(
import "test_foreign_params" for Point

var p1 = Point.new(0.0, 0.0, 0.0)
var p2 = Point.new(3.0, 4.0, 0.0)

System.print("p1 length: %(p1.length)")
System.print("p2 length: %(p2.length)")
System.print("distance: %(p1.distanceTo(p2))")
)WREN";

  result = vm.interpret("test10", test10Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Foreign parameter passing test");
  printf("\n");
  
  // Test 11: PointFloat class (single precision)
  {
    printf("Test 11: PointFloat class (single precision)\n");
    auto& floatMod = vm.module("test_float");
    auto pointFloat = floatMod.klass<PointFloat>("PointFloat");
    pointFloat.ctor<>();
    pointFloat.ctor<float, float, float>();
    pointFloat.func<&PointFloat::length>("length");
    pointFloat.func<&PointFloat::getX>("getX");
    pointFloat.func<&PointFloat::setX, float>("setX");
    pointFloat.varReadOnly<&PointFloat::y>("y");
    pointFloat.func<&PointFloat::distanceTo, PointFloat>("distanceTo");
    pointFloat.funcStatic<&PointFloat::origin>("origin");
  }

  const char* test11Source = R"WREN(
import "test_float" for PointFloat

var p1 = PointFloat.new(1.5, 2.5, 0.0)
var p2 = PointFloat.new(4.5, 6.5, 0.0)

System.print("p1 length: %(p1.length)")
System.print("p1 x: %(p1.getX)")
System.print("p2 y: %(p2.y) (expect 4)")

p1.setX(3.5)
System.print("p1 x after setX: %(p1.getX)")

System.print("p2 length: %(p2.length)")
System.print("distance: %(p1.distanceTo(p2))")

var origin = PointFloat.origin()
System.print("origin length: %(origin.length)")
)WREN";

  result = vm.interpret("test11", test11Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "PointFloat class test");
  printf("\n");

  class PointIntExt
  {
    public:
      static auto getX(PointInt& p) { return p.x; }
      static void setX(PointInt& p, int y) { p.x = y; }
      static auto getY(PointInt& p) { return p.y; }
  };

  // Test 12: PointFloatExt class (use external functions to access a class)
  {
    printf("Test 12: PointInt class (integer point, but access through external functions)\n");
    auto& floatMod = vm.module("test_int_ext");
    auto pointInt = floatMod.klass<PointInt>("PointInt");
    pointInt.ctor<>();
    pointInt.ctor<int, int, int>();
    pointInt.funcExt<&PointIntExt::getX>("getX");
    pointInt.propExt<&PointIntExt::getX, &PointIntExt::setX>("x");
    pointInt.propExtReadonly<&PointIntExt::getY>("y");
    // pointFloat.func<&PointIntExt::length>("length");
    // pointFloat.func<&PointIntExt::getX>("getX");
    // pointFloat.func<&PointIntExt::setX, float>("setX");
    // pointFloat.func<&PointIntExt::distanceTo, PointFloat>("distanceTo");
    // pointFloat.funcStatic<&PointFloat::origin>("origin");
  }
const char* test12Source = R"WREN(
import "test_int_ext" for PointInt

var p1 = PointInt.new(1, 2, 0.0)
var p2 = PointInt.new(4, 6, 0.0)

// System.print("p1 length: %(p1.length)")
System.print("p1 x: %(p1.getX)")
System.print("p1 y: %(p1.y)")

p1.x = 3
System.print("p1 x after x assignment: %(p1.getX)")

// System.print("p2 length: %(p2.length)")
// System.print("distance: %(p1.distanceTo(p2))")

// var origin = PointFloatExt.origin()
// System.print("origin length: %(origin.length)")
)WREN";

  result = vm.interpret("test12", test12Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "External class test");
  printf("\n");

  // Test 13: foreign / shared_ptr return values
  {
    printf("Test 13: foreign and shared_ptr return values\n");
    auto& mod = vm.module("test_returns");
    auto w = mod.klass<Widget>("Widget");
    w.func<&Widget::bump, int>("bump");
    w.func<&Widget::getChild>("getChild");
    w.func<&Widget::copy>("copy");
    w.funcStatic<&Widget::getRoot>("root");
    w.funcStatic<&Widget::make, int>("make");
    w.varReadOnly<&Widget::counter>("counter");
    w.varReadOnly<&Widget::tag>("tag");
  }

  // Prepared shared object graph: root(1) -> child(7). Wren mutations made
  // through getChild() must reach the C++ side (shared ownership), while
  // mutations of a copy() must not.
  auto sharedChild = std::make_shared<Widget>();
  sharedChild->counter = 7;
  sharedChild->tag = "child";
  Widget::root = std::make_shared<Widget>();
  Widget::root->counter = 1;
  Widget::root->child = sharedChild;

  const char* test13Source = R"WREN(
import "test_returns" for Widget

var root = Widget.root()
System.print("root counter: %(root.counter)")

var child = root.getChild()
System.print("child counter: %(child.counter)")
System.print("child tag: %(child.tag)")
child.bump(5)

var copy = root.copy()
System.print("copy counter: %(copy.counter)")
copy.bump(100)
System.print("root counter after copy bump: %(root.counter)")

var fresh = Widget.make(42)
System.print("fresh counter: %(fresh.counter)")
)WREN";

  result = vm.interpret("test13", test13Source);
  TEST_ASSERT(result == WREN_RESULT_SUCCESS, "Foreign/shared_ptr returns test");
  TEST_ASSERT(sharedChild->counter == 12, "shared_ptr return shares ownership");
  TEST_ASSERT(Widget::root->counter == 1, "foreign by-value return is a copy");
  Widget::root.reset();
  printf("\n");

#ifdef WREN_ENABLE_DEBUGGER
  // Test 14: Config::debug spins up a DAP debugger for this VM
  {
    printf("Test 14: debugger enabled through VM::Config\n");

    const int debugPort = 47129;
    wren::VM::Config debugConfig;
    debugConfig.debug = true;
    debugConfig.debugPort = debugPort;
    wren::VM debugVm(std::move(debugConfig));

    TEST_ASSERT(debugVm.debugger() != nullptr, "debugger attached when enabled");

    // Something must actually be serving on the port.
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(debugPort));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool listening = sock >= 0 &&
        connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    if (sock >= 0) close(sock);
    TEST_ASSERT(listening, "debug port accepts connections");

    // Scripts must run normally with the debug hook installed.
    auto& mod = debugVm.module("debug_test");
    mod.klass<Widget>("Widget").funcStatic<&Widget::make, int>("make")
        .func<&Widget::bump, int>("bump").varReadOnly<&Widget::counter>("counter");
    const char* debugSource = R"WREN(
import "debug_test" for Widget
var w = Widget.make(21)
w.bump(21)
System.print("bumped to %(w.counter)")
)WREN";
    WrenInterpretResult debugResult = debugVm.interpret("main", debugSource);
    TEST_ASSERT(debugResult == WREN_RESULT_SUCCESS, "interpret works with hook active");

    // Module path registration is accepted (no observable result; this just
    // exercises the passthrough).
    debugVm.registerModulePath("main", "/tmp/debug_test_main.wren");

    // Destructor detaches the hook before freeing the VM.
  }
  printf("\n");
#endif

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
