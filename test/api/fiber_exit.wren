// Tests that a fiber that exits using Fiber.exit() makes wrenCall() succeed
// and leaves the exit value in slot 0.
class Test {
  static exitWithValue() {
    Fiber.exit("exit value")
    System.print("not reached")
  }

  static exitWithNull() {
    Fiber.exit(null)
  }
}

// The C++ test harness then calls the methods above and prints:
// expect: result: success
// expect: value: exit value
// expect: result: success
// expect: type: 5
