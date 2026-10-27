// Foreign class bindings for test/api/foreign_class.wren, registered through
// the C++20 bindings API (wren.hpp) as the "foreign_classes" module. The Wren
// class declarations are served verbatim from the module source so the test
// script's expectations stay unchanged.

#include <cstdio>
#include <string>

#include "foreign_class.h"
#include "wren.hpp"

namespace {

int finalized = 0;

class Counter
{
public:
  void increment(double amount) { value += amount; }
  double getValue() const { return value; }

private:
  double value = 0.0;
};

class Point
{
public:
  Point() : coordinates{0.0, 0.0, 0.0} {}
  Point(double x, double y, double z) : coordinates{x, y, z} {}

  void translate(double x, double y, double z)
  {
    coordinates[0] += x;
    coordinates[1] += y;
    coordinates[2] += z;
  }

  std::string toString() const
  {
    char result[100];
    snprintf(result, sizeof(result), "(%g, %g, %g)",
        coordinates[0], coordinates[1], coordinates[2]);
    return result;
  }

private:
  double coordinates[3];
};

class Resource
{
public:
  ~Resource() { finalized++; }
};

// Payload-less binding targets: BadClass aborts in its allocator and
// ForeignClass only carries a foreign static.
struct BadClassTag {};
struct ForeignClassTag {};

void badClassAllocate(WrenVM* vm)
{
  wrenEnsureSlots(vm, 1);
  wrenSetSlotString(vm, 0, "Something went wrong");
  wrenAbortFiber(vm, 0);
}

int finalizedCount()
{
  return finalized;
}

} // namespace

void foreignClassSetup(WrenVM* vm)
{
  auto& module = vm->Module("foreign_classes");

  module.source(
      "class ForeignClass {\n"
      "  foreign static finalized\n"
      "}\n"
      "\n"
      "// Class with a default constructor.\n"
      "foreign class Counter {\n"
      "  construct new() {}\n"
      "  foreign increment(amount)\n"
      "  foreign value\n"
      "}\n"
      "\n"
      "// Foreign classes can inherit a class as long as it has no fields.\n"
      "class PointBase {\n"
      "  inherited() {\n"
      "    System.print(\"inherited method\")\n"
      "  }\n"
      "}\n"
      "\n"
      "// Class with non-default constructor.\n"
      "foreign class Point is PointBase {\n"
      "  construct new() {\n"
      "    System.print(\"default\")\n"
      "  }\n"
      "\n"
      "  construct new(x, y, z) {\n"
      "    System.print(\"%(x), %(y), %(z)\")\n"
      "  }\n"
      "\n"
      "  foreign translate(x, y, z)\n"
      "  foreign toString\n"
      "}\n"
      "\n"
      "// Class with a finalizer.\n"
      "foreign class Resource {\n"
      "  construct new() {}\n"
      "}\n"
      "\n"
      "// Class that aborts fiber\n"
      "foreign class BadClass {\n"
      "  construct new() {}\n"
      "}\n");

  auto counter = module.klass<Counter>("Counter");
  counter.noSource();
  counter.ctor<>();
  counter.func<&Counter::increment, double>("increment");
  counter.func<&Counter::getValue>("value");

  auto point = module.klass<Point>("Point");
  point.noSource();
  point.ctor<>();
  point.ctor<double, double, double>();
  point.func<&Point::translate, double, double, double>("translate");
  point.func<&Point::toString>("toString");

  auto resource = module.klass<Resource>("Resource");
  resource.noSource();
  resource.ctor<>();

  auto badClass = module.klass<BadClassTag>("BadClass");
  badClass.noSource();
  badClass.allocatorRaw(badClassAllocate);

  auto foreignClass = module.klass<ForeignClassTag>("ForeignClass");
  foreignClass.noSource();
  foreignClass.funcStatic<&finalizedCount>("finalized");
}
