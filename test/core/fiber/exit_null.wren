// Unlike abort(null), which does nothing, exit(null) still ends the fiber.
var fiber = Fiber.new {
  System.print("before") // expect: before
  Fiber.exit(null)
  System.print("not reached")
}

System.print(fiber.call()) // expect: null
System.print(fiber.isDone) // expect: true
