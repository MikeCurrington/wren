// Example: Using the new C++20 Wren API to bind a foreign class
#include <cstdio>
#include <string>
#include "wren.hpp"

// A C++ class we want to expose to Wren
class Vec3 {
public:
  double x, y, z;

  Vec3() : x(0), y(0), z(0) {}
  Vec3(double x, double y, double z) : x(x), y(y), z(z) {}

  double length() const {
    return sqrt(x*x + y*y + z*z);
  }

  double dot(const Vec3& other) const {
    return x*other.x + y*other.y + z*other.z;
  }

  double getX() const { return x; }
  void setX(double v) { x = v; }

  // Equality operators
  bool operator==(const Vec3& other) const {
    return x == other.x && y == other.y && z == other.z;
  }

  bool operator!=(const Vec3& other) const {
    return !(*this == other);
  }

  static Vec3 zero() { return Vec3(0, 0, 0); }
};

int main() {
  wren::VM::Config config;
  config.wrenConfig.writeFn = [](WrenVM*, const char* text) { printf("%s", text); };
  config.wrenConfig.errorFn = [](WrenVM*, WrenErrorType, const char* mod, int line, const char* msg) {
    fprintf(stderr, "[%s:%d] %s\n", mod ? mod : "?", line, msg);
  };
  wren::VM vm(std::move(config));

  // Register a module with a foreign class
  {
    auto& math = vm.module("math");
    auto vec = math.klass<Vec3>("Vec3");

    // Constructors
    vec.ctor<>();                    // Vec3.new()
    vec.ctor<double, double, double>(); // Vec3.new(_,_,_)

    // Methods — type-safe, no void* casting
    vec.func<&Vec3::length>("length");
    vec.func<&Vec3::dot, Vec3>("dot");

    // Equality operators
    vec.func<&Vec3::operator==, Vec3>("==");
    vec.func<&Vec3::operator!=, Vec3>("!=");

    // Property with getter/setter
    vec.prop<&Vec3::getX, &Vec3::setX>("x");

    // Static method
    vec.funcStatic<&Vec3::zero>("zero");

    // Auto-destruct on GC
    vec.finalize();
  } // Foreign<Vec3> destructor auto-registers the class

  // Generate and interpret the Wren source that uses our bindings
  const char* source = R"WREN(
import "math" for Vec3

var v = Vec3.new(3.0, 4.0, 0.0)
System.print("length = %(v.length)")  // expect 5.0

var a = Vec3.new(1.0, 2.0, 3.0)
var b = Vec3.new(4.0, 5.0, 6.0)
System.print("dot = %(a.dot(b))")     // expect 32.0

v.x = 6.0
System.print("x = %(v.x)")           // expect 6.0
System.print("new length = %(v.length)") // expect ~8.485...

// Test equality operators
var v1 = Vec3.new(1.0, 2.0, 3.0)
var v2 = Vec3.new(1.0, 2.0, 3.0)
var v3 = Vec3.new(4.0, 5.0, 6.0)

System.print("v1 == v2: %(v1 == v2)")  // expect true
System.print("v1 != v3: %(v1 != v3)")  // expect true
System.print("v1 == v3: %(v1 == v3)")  // expect false
System.print("v1 != v2: %(v1 != v2)")  // expect false
)WREN";

  auto result = vm.interpret("main", source);
  if (result != WREN_RESULT_SUCCESS) {
    printf("Wren interpretation failed.\n");
    return 1;
  }

  printf("Success!\n");
  return 0;
}