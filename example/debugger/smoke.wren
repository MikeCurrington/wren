// Driven by util/dap_smoke_test.py, which breaks on line 5.
var total = 0

for (i in 1..3) {
  total = total + i
}

var numbers = [1, 2, 3]
System.print("total is %(total)")
