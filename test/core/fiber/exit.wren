var fiber = Fiber.new {
  System.print("fiber") // expect: fiber
  Fiber.exit("result")
  System.print("not reached")
}

System.print(fiber.call()) // expect: result
System.print(fiber.isDone) // expect: true
System.print(fiber.error) // expect: null
