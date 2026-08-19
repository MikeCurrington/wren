var fiber = Fiber.new {
  Fiber.exit("done")
}

System.print(fiber.call()) // expect: done
fiber.call() // expect runtime error: Cannot call a finished fiber.
