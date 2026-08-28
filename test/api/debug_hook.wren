class Debug {
  foreign static install()
  foreign static uninstall()
}

class Foo {
  static bar(a) {
    return a * 2
  }
}

Debug.install()
var x = 1
x = x + 1
Foo.bar(x)

// The hook reports each line transition of the module's top-level code, the
// method body it calls into, and the statement after the call returns.
// expect: line 12 in (script), 1 frame(s)
// expect: line 13 in (script), 1 frame(s)
// expect: line 14 in (script), 1 frame(s)
// expect: line 15 in (script), 1 frame(s)
// expect: line 8 in bar(_), 2 frame(s)
// expect: bar has 2 local(s)
// expect: bar's first local slot after the receiver is 2
// expect: module variable x is 2
// expect: line 29 in (script), 1 frame(s)

Debug.uninstall()
System.print(x) // expect: 2
