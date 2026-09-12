# Debugging Wren from VS Code

This fork ships a source-level debugger for Wren scripts that speaks the
[Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
(DAP) over a local TCP port, so VS Code (or any other DAP client) can set
breakpoints, step through code, and inspect the call stack and variables of a
running Wren VM — including a VM embedded inside a host application.

## Pieces

| Component | Location | Role |
|---|---|---|
| VM debug hook | `src/vm/wren_debug.cpp`, `src/include/wren.h` | Fires on each source line change; frame/local/module-variable inspection APIs |
| Debugger interface | `src/include/wren_debugger.h` | Public `wren::debug::Debugger` API for hosts |
| Debugger session | `src/debugger/wren_debugger.cpp` | Breakpoints, stepping, pause/resume, stack & variables over DAP |
| Transport | `src/debugger/dap_io.{h,cpp}`, `src/debugger/wren_json.{h,cpp}` | DAP framing over TCP, minimal JSON |
| Example host | `example/debugger/main.cpp` | Standalone runnable host (`wren_debug_example`) |
| VS Code bridge | `extensions/vscode-wren-debug/` | Contributes the `wren` debug type; points VS Code at the host's port |

The library is built as the `wren_debugger` static target behind the CMake
option `WREN_DEBUGGER` (on by default).

## How it works

The interpreter's dispatch loop already had a per-instruction choke point;
`wrenVmDebugHook()` now sits behind it and invokes the host's debug hook once
per *line transition* — including backward jumps, so loop bodies whose code
lives on one line still report. The check costs nothing when no hook is
installed.

When the session decides to stop (entry, breakpoint, step, or pause), the VM
thread blocks inside the hook, exactly like a foreign method call: API slots
are wired to the top of the fiber's stack, and the hook serves DAP requests
that need live state (`stackTrace`, `scopes`, `variables`) until the client
resumes. Connection I/O runs on background threads, so a frozen VM never
blocks the socket.

Breakpoints are stored as *(module name, line)* pairs and checked at runtime,
which means a breakpoint placed in a module that hasn't been imported yet
still works once it loads.

## Using the example host

```sh
# from projects/cmake
cmake . -B build && make -C build wren_debug_example
./build/wren_debug_example --port 4711 --wait-ms 60000 myscript.wren
```

`--wait-ms` waits up to that long for a client to attach before running, so
the first line stops with an entry stop. Then in VS Code, with the
`vscode-wren-debug` extension installed (see its README), use:

```json
{
  "version": "0.2.0",
  "configurations": [
    { "type": "wren", "request": "attach", "name": "Attach to Wren VM", "port": 4711 }
  ]
}
```

## Embedding in a host application

```c++
#include "wren_debugger.h"

wren::debug::Debugger debugger;
debugger.attach(vm, 4711);                    // listen + install hook

// As modules are loaded, tell the debugger where each one came from so DAP
// source paths can be mapped to module names and back:
debugger.registerModulePath("main", "/abs/path/main.wren");

// Optional: hold the program until the client finishes configuring, then
// stop on the first line of code:
debugger.waitForConfiguration(30000);

WrenInterpretResult result = wrenInterpret(vm, "main", source);

// Tell the client the program finished:
debugger.notifyExecutionEnded();
```

Route your `writeFn` through `debugger.writeOutput(text)` and `System.print`
output appears in VS Code's debug console.

The host application owns the threading contract: the VM must run on one
thread (as usual), and the debugger owns the others.

### wren.hpp (C++20 bindings)

When a host links `wren_debugger` (which publicly defines
`WREN_ENABLE_DEBUGGER`), `wren::VM::Config` gains a debug switch:

```c++
wren::VM::Config config;
config.wrenConfig.writeFn = myWrite;   // still called; also mirrored to VS Code
config.debug = true;                   // serve DAP on config.debugPort (4711)
wren::VM vm(std::move(config));

vm.registerModulePath("main", "/abs/path/main.wren");
vm.interpret("main", source);
// For one-shot hosts: vm.debugger()->notifyExecutionEnded();
```

Without `wren_debugger` linked, `Config::debug` still exists but the debug
code is compiled out, so plain embedders take no new dependency.

## VM debug API

For hosts that want to implement their own debugging policy instead of using
the DAP session, the VM exposes a low-level hook (see `wren.h`):

- `wrenSetDebugHook(vm, fn, userData)` — calls `fn` on every source line
  transition while the VM runs. The hook may block to pause the VM.
- `wrenDebugGetFrameCount`, `wrenDebugGetFrameInfo` — the call stack, with
  module, function, and line per frame (frame 0 = innermost).
- `wrenDebugGetLocalCount`, `wrenDebugGetLocal` — a frame's stack slots.
- `wrenDebugGetModuleVariableCount/Name`, `wrenDebugGetModuleVariable` — the
  executing module's top-level variables by name.

Inside the hook, the standard slot API (`wrenEnsureSlots`, `wrenGetSlotDouble`,
...) works on the paused fiber, just like in a foreign method.

## Current limitations

- Locals that are expression temporaries or out of scope have no name and are
  shown by slot position (`slot0`, ...) after the named locals. Instance
  fields of classes the VM compiled are shown by name; other classes fall
  back to `field0`, .... Field names are matched by class name, so two
  different modules defining the same class name show the last-defined
  names.
- No expression evaluation / watch / REPL while paused.
- No conditional or exception breakpoints.
- The call stack spans the running fiber and every fiber that resumed it via
  `Fiber.call()` (or `try()`), so the resumer's frames and variables are
  visible across a `yield`. `Fiber.transfer()` records no link back, so
  transferred fibers show only their own frames.
- One VM (and one DAP client) per `Debugger` instance.
