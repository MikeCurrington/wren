// Drains a generator fiber. Measures the cost of a Fiber.call/Fiber.yield
// round trip: one million resumes, one value passed out per yield.
var fiber = Fiber.new {
  var i = 0
  while (true) {
    i = i + 1
    Fiber.yield(i)
  }
}

var sum = 0
var start = System.clock
for (i in 1..1000000) {
  sum = sum + fiber.call()
}
System.print(sum)
System.print("elapsed: %(System.clock - start)")
