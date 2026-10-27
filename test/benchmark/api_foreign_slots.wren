// Measures slot marshalling across the foreign boundary. A 100 element
// list is read element-wise by a foreign method (Wren -> C++), and a new
// 100 element list is built by one (C++ -> Wren) every iteration.
class Slots {
  foreign static sum(list)
  foreign static build(n)
}

var data = []
for (i in 1..100) {
  data.add(i * 1.0)
}

var total = 0
var start = System.clock
for (i in 1..20000) {
  total = total + Slots.sum(data)
  total = total + Slots.build(100)[0]
}
System.print(total)
System.print("elapsed: %(System.clock - start)")
