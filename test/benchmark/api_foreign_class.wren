// Measures foreign class overhead: instance method dispatch, foreign data
// access, and return marshalling across the language boundary.
foreign class Accumulator {
  construct new(start) {}
  foreign inc()
  foreign value
}

var accumulator = Accumulator.new(0)

var total = 0
var start = System.clock
for (i in 1..1000000) {
  total = total + accumulator.inc()
  total = total + accumulator.value
}
System.print(total)
System.print("elapsed: %(System.clock - start)")
