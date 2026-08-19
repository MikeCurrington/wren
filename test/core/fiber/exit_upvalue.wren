// A closure created in the fiber should still see the value its captured
// local had when the fiber exited.
var closure = null

var fiber = Fiber.new {
  var local = "before exit"
  closure = Fn.new { local }
  Fiber.exit("exited")
  local = "never"
}

System.print(fiber.call()) // expect: exited
System.print(closure.call()) // expect: before exit
