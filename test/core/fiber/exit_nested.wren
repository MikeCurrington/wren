class ExitNested {
  construct new() {}

  middle() {
    Fiber.exit("from middle")
  }

  outer() {
    middle()
    System.print("not reached")
  }
}

var fiber = Fiber.new {
  ExitNested.new().outer()
}

System.print(fiber.call()) // expect: from middle
System.print(fiber.isDone) // expect: true
