#pragma once
#ifndef wren_debugger_h
#define wren_debugger_h

#include <memory>
#include <string>

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
//   wren::debug::Debugger debugger;
//   debugger.attach(vm, 4711);                 // listens + installs hook
//   debugger.registerModulePath("main", path); // module <-> file mapping
//   debugger.waitForConfiguration();           // optional "debug from start"
//   wrenInterpret(vm, "main", source);
//   debugger.notifyExecutionEnded();
//
// The VM thread blocks inside the debug hook whenever the client stops at
// a breakpoint/step, while background threads serve the DAP connection.
// Requests that inspect VM state are handed to the VM thread and answered
// from inside the hook.
namespace wren
{
namespace debug
{
  class Debugger
  {
    public:
      Debugger();
      ~Debugger();

      Debugger(const Debugger&) = delete;
      Debugger& operator=(const Debugger&) = delete;

      // Starts listening on 127.0.0.1:[port] and installs the VM debug hook.
      // Returns false if the socket could not be bound.
      bool attach(WrenVM* vm, int port);

      // Removes the debug hook and stops serving. Must not be called from a
      // different thread while the VM is paused inside the debug hook; call
      // it after wrenInterpret()/wrenCall() has returned.
      void detach();

      // Tells the debugger that Wren module [module] was loaded from [path].
      // Used to translate between DAP source paths and Wren module names, in
      // both directions. Call this as modules are loaded.
      void registerModulePath(const std::string& module,
                              const std::string& path);

      // Blocks until a client has connected and sent its initial
      // configuration (breakpoints and so on), or until [timeoutMs] elapses
      // (pass a negative value to wait indefinitely). Hosts that want
      // "debug from the first line" behavior call this before running any
      // code. Returns true if the client finished configuring.
      bool waitForConfiguration(int timeoutMs);

      // The host must call this when the interpreter returns so that the
      // client learns the debuggee finished.
      void notifyExecutionEnded();

      // Whether the first line of executed code stops the VM ("entry" stop).
      // On by default when the host waits for configuration before starting.
      // May also be set by clients through their launch configuration.
      void setStopOnEntry(bool stopOnEntry);

      // Sends [text] to the client's debug console as a stdout "output"
      // event. Hosts call this from their writeFn so System.print() shows up
      // in VS Code.
      void writeOutput(const std::string& text);

    private:
      struct Impl;
      std::unique_ptr<Impl> impl_;
  };
}
}

#endif
