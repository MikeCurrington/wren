#pragma once
#ifndef wren_debugger_h
#define wren_debugger_h

#include <functional>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

#include "wren.h"

// A source-level debugger for a Wren VM that speaks the Debug Adapter
// Protocol (DAP) over a local TCP port, so that VS Code (or any other DAP
// client) can attach to a host application and debug the Wren scripts it
// runs: breakpoints, pause/continue, stepping, call stack, and variables.
//
// The implementation lives in the wren_debugger library (src/debugger/).
//
// Usage from the host:
//
//   auto debugger = wren::debug::MakeDebugger();
//   debugger->attach(vm, 4711);                 // listens + installs hook
//   debugger->registerModulePath("main", path); // module <-> file mapping
//   debugger->waitForConfiguration();           // optional "debug from start"
//   wrenInterpret(vm, "main", source);
//   debugger->notifyExecutionEnded();
//
// The VM thread blocks inside the debug hook whenever the client stops at
// a breakpoint/step, while background threads serve the DAP connection.
// Requests that inspect VM state are handed to the VM thread and answered
// from inside the hook.
namespace wren
{
namespace debug
{
  // Identifies one inspectable foreign (C++-backed) value. For a Wren
  // foreign instance the debugger fills [data] with the instance's opaque
  // data bytes and leaves [tag] null; the host's provider unwraps them. For
  // nested values the provider passes back the data/tag/keepAlive it handed
  // out with a child variable. [keepAlive] pins the memory the node points
  // at for as long as the debugger holds the node.
  struct ForeignNode
  {
    const void* data = nullptr;
    const std::type_info* tag = nullptr;
    std::shared_ptr<void> keepAlive;
    std::string wrenClassName;  // set for Wren foreign instances
  };

  // One child variable of a foreign value, as described by the provider.
  // A variable with non-null [data] and [tag] is itself expandable: the
  // provider will be called again with a node built from those fields.
  struct ForeignVariable
  {
    std::string name;
    std::string value;  // display string for the value column
    std::string type;   // DAP type column (may be empty)
    const void* data = nullptr;
    const std::type_info* tag = nullptr;
    std::shared_ptr<void> keepAlive;
  };

  // Host-provided introspection for foreign values. Both functions run on
  // the VM thread while the VM is paused, and must only read state - never
  // run Wren code. [summarize] returns the value-column string for a foreign
  // value, or "" when the host cannot describe that type: the debugger then
  // falls back to "ClassName @ 0x..." and treats the value as a leaf.
  // [children] lists the variables of an expandable foreign value.
  struct ForeignProvider
  {
    std::function<std::string(const ForeignNode& node)> summarize;
    std::function<std::vector<ForeignVariable>(const ForeignNode& node)> children;
  };

  class Debugger
  {
    public:
      Debugger();
      virtual ~Debugger();

      Debugger(const Debugger&) = delete;
      Debugger& operator=(const Debugger&) = delete;

      // Starts listening on 127.0.0.1:[port] and installs the VM debug hook.
      // Returns false if the socket could not be bound.
      virtual bool attach(WrenVM* vm, int port) = 0;

      // Removes the debug hook and stops serving. Must not be called from a
      // different thread while the VM is paused inside the debug hook; call
      // it after wrenInterpret()/wrenCall() has returned.
      virtual void detach() = 0;

      // Tells the debugger that Wren module [module] was loaded from [path].
      // Used to translate between DAP source paths and Wren module names, in
      // both directions. Call this as modules are loaded.
      virtual void registerModulePath(const std::string& module,
                                      const std::string& path) = 0;

      // Blocks until a client has connected and sent its initial
      // configuration (breakpoints and so on), or until [timeoutMs] elapses
      // (pass a negative value to wait indefinitely). Hosts that want
      // "debug from the first line" behavior call this before running any
      // code. Returns true if the client finished configuring.
      virtual bool waitForConfiguration(int timeoutMs) = 0;

      // The host must call this when the interpreter returns so that the
      // client learns the debuggee finished.
      virtual void notifyExecutionEnded() = 0;

      // Whether the first line of executed code stops the VM ("entry" stop).
      // On by default when the host waits for configuration before starting.
      // May also be set by clients through their launch configuration.
      virtual void setStopOnEntry(bool stopOnEntry) = 0;

      // Sends [text] to the client's debug console as a stdout "output"
      // event. Hosts call this from their VM's write() override so System.print() shows up
      // in VS Code.
      virtual void writeOutput(const std::string& text) = 0;

      // Installs host-provided introspection for foreign values, so that
      // variables holding foreign class instances can be summarized and
      // expanded in the variables pane. Call once during host startup,
      // before a client attaches.
      virtual void setForeignProvider(ForeignProvider provider) = 0;
  };

  std::unique_ptr<Debugger> MakeDebugger();
}
}

#endif
