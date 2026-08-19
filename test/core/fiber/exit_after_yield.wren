var fiber = Fiber.new {
  System.print("before yield") // expect: before yield
  Fiber.yield()
  System.print("after yield") // expect: after yield
  Fiber.exit("result")
}

fiber.call()
System.print(fiber.call()) // expect: result
