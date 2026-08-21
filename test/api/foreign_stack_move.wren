// Regression test.
//
// A foreign method that grows the API stack with wrenEnsureSlots() forces the
// fiber's value stack to be reallocated. The interpreter used to keep its
// cached stack pointer (stackStart) across the foreign call, so when the
// enclosing block returned, its result was written through a dangling pointer
// into the freed old stack buffer. The caller then found the block's own
// closure still sitting in the result slot:
//
//   boards.map {|b| b.signal }.toList   // first element was <fn>

foreign class Signal {
  construct new(value) {}
  foreign value
}

foreign class Board {
  construct new(id) {}
  foreign signal
}

class Test {
  static mapped() {
    var boards = [Board.new(1), Board.new(2), Board.new(3), Board.new(4)]
    var signals = boards.map {|b| b.signal }.toList
    for (s in signals) System.print(s is Signal)
    // expect: true
    // expect: true
    // expect: true
    // expect: true
    for (s in signals) System.print(s.value)
    // expect: 1
    // expect: 2
    // expect: 3
    // expect: 4
  }

  static mappedInFiber() {
    var boards = [Board.new(5), Board.new(6)]
    var f = Fiber.new {
      var signals = boards.map {|b| b.signal }.toList
      for (s in signals) System.print(s is Signal)
      // expect: true
      // expect: true
    }
    f.call()
  }

  static manual() {
    var boards = [Board.new(7), Board.new(8)]
    var signals = []
    for (b in boards) signals.add(b.signal)
    for (s in signals) System.print(s is Signal)
    // expect: true
    // expect: true
  }
}
