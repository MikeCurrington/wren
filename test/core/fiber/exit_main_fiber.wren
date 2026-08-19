System.print("before") // expect: before
Fiber.exit("bye")
System.print("after")
