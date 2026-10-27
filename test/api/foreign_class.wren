import "foreign_classes" for Counter, Point, Resource, BadClass, ForeignClass

var counter = Counter.new()
System.print(counter.value) // expect: 0
counter.increment(3.1)
System.print(counter.value) // expect: 3.1
counter.increment(1.2)
System.print(counter.value) // expect: 4.3

var p = Point.new(1, 2, 3) // expect: 1, 2, 3
System.print(p) // expect: (1, 2, 3)
p.translate(3, 4, 5)
System.print(p) // expect: (4, 6, 8)

p = Point.new() // expect: default
System.print(p) // expect: (0, 0, 0)

p.inherited() // expect: inherited method

var error = Fiber.new {
  class Subclass is Point {}
}.try()
System.print(error) // expect: Class 'Subclass' cannot inherit from foreign class 'Point'.

var resources = [
  Resource.new(),
  Resource.new(),
  Resource.new()
]

System.gc()
System.print(ForeignClass.finalized) // expect: 0

resources.removeAt(-1)

System.gc()
System.print(ForeignClass.finalized) // expect: 1

resources.clear()

System.gc()
System.print(ForeignClass.finalized) // expect: 3

error = Fiber.new {
  BadClass.new()
}.try()
System.print(error) // expect: Something went wrong
