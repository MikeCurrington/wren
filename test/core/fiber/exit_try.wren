var fiber = Fiber.new {
  System.print("before") // expect: before
  Fiber.exit("value")
}

System.print(fiber.try()) // expect: value
System.print(fiber.isDone) // expect: true
System.print(fiber.error) // expect: null
