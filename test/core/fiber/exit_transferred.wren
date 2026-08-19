// A transferred-to fiber that exits has no caller, so the interpreter stops,
// just like when a transferred-to fiber returns.
var fiber = Fiber.new {
  System.print("in fiber")
  Fiber.exit("done")
}

System.print("before") // expect: before
fiber.transfer()       // expect: in fiber
System.print("not reached")
