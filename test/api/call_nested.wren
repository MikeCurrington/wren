// Tests for re-entrant calls into the VM from inside foreign methods, using
// wrenBeginCall() and wrenEndCall(). The static run*() methods are invoked
// from C (see call_nested.cpp); each one exercises a different scenario:
//
// * Calling back into Wren with wrenCall() from inside a foreign method.
// * Calling wrenInterpret() from inside a foreign method.
// * Multiple levels of nesting (Wren -> C -> Wren -> C -> Wren).
// * Garbage collection while fibers are suspended in foreign calls.
// * Stack growth and reallocation during nested calls.
// * Handling errors from nested calls in C, and propagating them outward.
// * Fibers exiting from nested calls.
// * Guarding against resuming a fiber that is suspended in a foreign call.

class Nested {
  // The foreign methods under test. Each one calls back into Wren.
  foreign static callWren(a, b)
  foreign static interpret(a)
  foreign static forgetEnd(a)
  foreign static twoCalls(a, b)
  foreign static level0(a)
  foreign static gcStress(a)
  foreign static stackMove(a)
  foreign static errorHandled()
  foreign static errorPropagate()
  foreign static fiberExit()
  foreign static nestedSuspend()
  foreign static guardCall(fiber)
  foreign static guardTransfer(fiber)

  // Foreign helpers invoked by the nested Wren code itself.
  foreign static gc()
  foreign static slots(n)
  foreign static level1Foreign(x)

  // Plain Wren methods invoked from C via nested wrenCall()s.
  static add(a, b) {
    return a + b
  }

  static multiply(a, b) {
    System.print("multiply %(a) %(b)")
    return a * b
  }

  static double(n) {
    System.print("doubling %(n)")
    return n * 2
  }

  static thrower() {
    Fiber.abort("nested error") // expect handled runtime error: nested error
  }

  static exitValue() {
    Fiber.exit("exited value")
  }

  static allocate(n) {
    var strings = []
    for (i in 0...n) strings.add("string %(i)")

    // Force a full garbage collection while a fiber is suspended in a
    // foreign call waiting for this code to finish.
    gc()
    return strings.count
  }

  static stackGrow(n) {
    return slots(n)
  }

  static level1(x) {
    return level1Foreign(x) + 100
  }

  static attemptCall(outer) {
    outer.call()
  }

  static attemptTransfer(outer) {
    outer.transfer()
  }

  static suspend() {
    Fiber.yield()
    System.print("never resumed")
    return "not this"
  }

  static runAll() {
    runCallWren()
    runInterpret()
    runForgetEnd()
    runTwoCalls()
    runLevel0()
    runGcStress()
    runStackMove()
    runErrorHandled()
    runFiberExit()
    runSuspend()
    runGuards()
    runConstruct()
  }

  static runCallWren() {
    // The foreign method prints its own arguments after the nested call
    // completes to verify they survived.
    System.print(Nested.callWren(3, 4))
    // expect: own args after nested call: 3 4
    // expect: 7
  }

  static runInterpret() {
    System.print(Nested.interpret("argument"))
    // expect: [nested interpret]
    // expect: own arg after nested interpret: argument
    // expect: argument
  }

  static runForgetEnd() {
    // The foreign method forgets to call wrenEndCall(). The VM should clean
    // up after it, and since the foreign method never wrote its return slot,
    // it returns its receiver, the class itself.
    System.print(Nested.forgetEnd(5))
    // expect: doubling 5
    // expect: Nested
  }

  static runTwoCalls() {
    System.print(Nested.twoCalls(2, 3))
    // expect: multiply 2 3
    // expect: results: 5 6
    // expect: 6
  }

  static runLevel0() {
    System.print(Nested.level0(1))
    // expect: multiply 1 2
    // expect: 102
  }

  static runGcStress() {
    // These locals live on this fiber's stack while it is suspended in the
    // foreign call, so the garbage collector must keep them alive.
    var keep1 = "outer one"
    var keep2 = "outer two"
    System.print(Nested.gcStress("outer arg"))
    // expect: nested allocated 100
    // expect: own arg after gc: outer arg
    // expect: 9
    System.print(keep1) // expect: outer one
    System.print(keep2) // expect: outer two
  }

  static runStackMove() {
    System.print(Nested.stackMove("survivor"))
    // expect: nested slots 60
    // expect: own arg after move: survivor
    // expect: 8
  }

  static runErrorHandled() {
    // The nested call aborts, but the C code handles it and the outer call
    // completes normally.
    System.print(Nested.errorHandled())
    // expect: nested call failed as expected
    // expect: handled
  }

  static runFiberExit() {
    System.print(Nested.fiberExit())
    // expect: nested exit result: 0
    // expect: exited value
  }

  static runSuspend() {
    // The nested call suspends its fiber. Only the innermost interpreter
    // stops; the foreign method still completes normally. (The suspended
    // fiber is abandoned, which is the host's problem to manage.)
    System.print(Nested.nestedSuspend())
    // expect: nested suspended
  }

  static runGuards() {
    // This fiber is suspended inside guardCall()'s foreign method while the
    // nested code runs, so resuming it must be an error, not a crash. The
    // nested call aborts with a runtime error; the C code observes it and
    // reports that the call was blocked.
    System.print(Nested.guardCall(Fiber.current))
    // expect: call blocked
    System.print(Nested.guardTransfer(Fiber.current))
    // expect: transfer blocked
  }

  static runConstruct() {
    // The nesting happens inside a foreign class's allocator.
    var f = NestedForeign.new(21)
    System.print(f.value)
    // expect: doubling 21
    // expect: 42
  }

  static runErrorPropagate() {
    // This aborts the fiber; the C test driver checks the result.
    Nested.errorPropagate()
  }
}

// A foreign class whose allocator calls back into Wren. The value stored in
// it is computed by the nested call.
foreign class NestedForeign {
  construct new(x) {}
  foreign value
}
