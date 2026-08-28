#pragma once
#ifndef wren_debugger_h
#define wren_debugger_h

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "dap_io.h"
#include "wren.h"

namespace wren
{
namespace debug
{
  // A source-level debugger for a Wren VM that speaks the Debug Adapter
  // Protocol over a local TCP port, so that VS Code (or any other DAP client)
  // can attach to a host application and debug the Wren scripts it runs.
  //
  // Usage from the host:
  //
  //   Debugger debugger;
  //   debugger.attach(vm, 4711);                 // listens + installs hook
  //   debugger.registerModulePath("main", path); // module <-> file mapping
  //   debugger.waitForConfiguration();           // optional "debug from start"
  //   wrenInterpret(vm, "main", source);
  //   debugger.notifyExecutionEnded();
  //
  // The VM thread blocks inside the debug hook whenever the client stops at
  // a breakpoint/step, while a background thread serves the DAP connection.
  // Requests that inspect VM state are handed to the VM thread and answered
  // from inside the hook.
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
      // configuration (breakpoints and so on), or until [timeoutMs] elapses,
      // whichever comes first. Hosts that want "debug from the first line"
      // behavior call this before running any code. Returns true if the
      // client finished configuring.
      bool waitForConfiguration(int timeoutMs);

      // The host must call this when the interpreter returns so that the
      // client learns the debuggee finished.
      void notifyExecutionEnded();

      // Whether the first line of executed code stops the VM. On by default
      // when the host waits for configuration before starting.
      void setStopOnEntry(bool stopOnEntry);

      // Sends [text] to the client's debug console as a stdout "output"
      // event. Hosts call this from their writeFn so System.print() shows up
      // in VS Code.
      void writeOutput(const std::string& text);

    private:
      // How the VM should run after the client resumes it.
      enum class StepMode
      {
        None,   // run freely
        In,     // stop at the next line event in any frame
        Over,   // stop at the next line event at or above the starting depth
        Out     // stop after returning above the starting depth
      };

      // A DAP request that has to be answered on the VM thread because it
      // inspects live paused state (stack, variables).
      struct VmRequest
      {
        Json request;
      };

      // A registered "variablesReference": the client expands these lazily
      // to list the contents of a scope or container value.
      struct VariableRef
      {
        enum class Kind
        {
          Locals,           // [frame]'s stack slots
          ModuleVariables,  // [frame]'s module's top-level variables
          List,
          Map,
          Instance
        };

        Kind kind = Kind::Locals;
        int frame = 0;                 // public frame index (0 = innermost)
        WrenHandle* value = nullptr;   // for container kinds
      };

      // ---- runs on the VM thread ----
      static void hookThunk(WrenVM* vm, WrenDebugEvent event, void* userData);
      void onLineEvent();
      void pauseLoop();
      void runVmRequest(const VmRequest& request);

      // ---- runs on the DAP thread ----
      void handleMessage(Json message);
      std::string moduleForPath(const std::string& path);
      void sendResponse(const Json& request, Json body);
      void sendErrorResponse(const Json& request, const std::string& message);
      void sendEvent(const std::string& event, Json body);
      void resume(StepMode mode);

      // ---- either thread ----
      void send(Json message);

      // ---- VM-state helpers, must run with the VM paused ----
      Json buildStackFrames();
      Json buildScopes(int frame);
      Json buildVariables(int reference);
      int makeVariableRef(VariableRef::Kind kind, int frame, WrenHandle* value);
      void clearVariableRefs();
      void setSlotFromRef(const VariableRef& ref, int slot);
      bool isBreakpointAt(const WrenDebugFrameInfo& info);

      WrenVM* vm_ = nullptr;
      DapConnection connection_;

      std::mutex mutex_;
      std::condition_variable cv_;
      std::condition_variable configurationCv_;

      std::atomic<int> sequence_{1};

      // Breakpoints keyed by resolved Wren module name.
      std::map<std::string, std::set<int>> breakpoints_;

      // Bidirectional module name <-> source path mapping.
      std::map<std::string, std::string> moduleToPath_;
      std::map<std::string, std::string> pathToModule_;

      // Pause/stepping state. Guarded by [mutex_].
      bool paused_ = false;
      bool pauseRequested_ = false;
      bool stopOnEntry_ = false;
      bool configurationDone_ = false;
      StepMode stepMode_ = StepMode::None;
      int stepFromFrameCount_ = 0;
      std::string stopReason_;
      std::deque<VmRequest> vmRequests_;

      // variablesReference bookkeeping. Valid only while paused.
      std::map<int, VariableRef> variableRefs_;
      std::map<int, std::string> sourceReferences_;
      int nextVariableRef_ = 1;
      int nextSourceReference_ = 1;
  };
}
}

#endif
