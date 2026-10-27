// Drains a generator fiber with a value passed in on every resume and a
// value yielded out, where the generator state lives in captured upvalues.
// Measures the Fiber.call/Fiber.yield round trip overhead with parameter
// passing and closure captures on top of the bare resume cost.
class Generator {
  static create(start, step) {
    return Fiber.new {
      var current = start
      while (true) {
        var input = Fiber.yield(current)
        current = current + input + step
      }
    }
  }
}

var generator = Generator.create(1, 2)

var sum = 0
var start = System.clock
for (i in 1..1000000) {
  sum = sum + generator.call(1)
}
System.print(sum)
System.print("elapsed: %(System.clock - start)")
