#include "wren_debugger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "dap_io.h"

// The debugger reads live VM state directly, exactly like the VM's own stack
// trace printer does.
#include "wren_vm.h"

namespace wren
{
namespace debug
{
  namespace
  {
    // Maximum characters of a value shown in the variables pane before it
    // is truncated.
    const int kMaxValueLength = 120;

    std::string formatNumber(double value)
    {
      char buffer[32];
      snprintf(buffer, sizeof(buffer), "%g", value);
      return buffer;
    }

    std::string truncate(const std::string& text, int maxLength)
    {
      if (static_cast<int>(text.size()) <= maxLength) return text;
      return text.substr(0, static_cast<size_t>(maxLength)) + "...";
    }

    // Formats a value for display in the variables pane. Purely structural:
    // it never runs Wren code, so it cannot perturb the paused program.
    std::string formatValue(Value value)
    {
      if (IS_NUM(value)) return formatNumber(AS_NUM(value));
      if (IS_BOOL(value)) return AS_BOOL(value) ? "true" : "false";
      if (IS_NULL(value)) return "null";
      if (!IS_OBJ(value)) return "?";

      Obj* obj = AS_OBJ(value);
      switch (obj->type)
      {
        case OBJ_STRING:
          return truncate("\"" + std::string(AS_STRING(value)->value) + "\"",
                          kMaxValueLength);

        case OBJ_LIST:
          return "list [" + std::to_string(AS_LIST(value)->elements.count) + "]";

        case OBJ_MAP:
          return "map [" + std::to_string(AS_MAP(value)->count) + "]";

        case OBJ_RANGE:
        {
          ObjRange* range = AS_RANGE(value);
          return formatNumber(range->from) +
              (range->isInclusive ? ".." : "...") + formatNumber(range->to);
        }

        case OBJ_CLASS:
          return "class " + std::string(AS_CLASS(value)->name->value);

        case OBJ_FN:
        {
          ObjFn* fn = AS_FN(value);
          return "fn " + (fn->debug->name != nullptr
              ? std::string(fn->debug->name) : std::string("(anonymous)"));
        }

        case OBJ_CLOSURE:
        {
          ObjFn* fn = AS_CLOSURE(value)->fn;
          return "fn " + (fn->debug->name != nullptr
              ? std::string(fn->debug->name) : std::string("(anonymous)"));
        }

        case OBJ_FIBER:
          return "fiber";

        case OBJ_INSTANCE:
        {
          ObjClass* classObj = obj->classObj;
          return std::string(classObj->name->value) + " {" +
              std::to_string(classObj->numFields) + " field(s)}";
        }

        case OBJ_FOREIGN:
        {
          ObjClass* classObj = obj->classObj;
          char buffer[64];
          snprintf(buffer, sizeof(buffer), " @ %p", AS_FOREIGN(value)->data);
          return std::string(classObj->name->value) + buffer;
        }

        default:
          return "[object]";
      }
    }

    // Whether a value can be expanded into child variables.
    bool isExpandable(Value value)
    {
      if (!IS_OBJ(value)) return false;
      ObjType type = AS_OBJ(value)->type;
      return type == OBJ_LIST || type == OBJ_MAP || type == OBJ_INSTANCE;
    }

    std::string basename(const std::string& path)
    {
      size_t slash = path.find_last_of('/');
      if (slash == std::string::npos) return path;
      return path.substr(slash + 1);
    }
  }

  // All debugger state and behavior. The public header exposes only an
  // opaque impl pointer so that hosts including wren_debugger.h don't pull
  // in any DAP or VM internals.
  struct Debugger::Impl
  {
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

    Impl() = default;

    // ---- lifecycle (called by the public forwarding methods) ----
    bool attach(WrenVM* vm, int port);
    void detach();
    void registerModulePath(const std::string& module,
                            const std::string& path);
    bool waitForConfiguration(int timeoutMs);
    void notifyExecutionEnded();
    void setStopOnEntry(bool stopOnEntry);
    void writeOutput(const std::string& text);

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

  Debugger::Debugger()
    : impl_(new Impl())
  {}

  Debugger::~Debugger()
  {
    detach();
  }

  bool Debugger::attach(WrenVM* vm, int port)
  {
    return impl_->attach(vm, port);
  }

  void Debugger::detach()
  {
    impl_->detach();
  }

  void Debugger::registerModulePath(const std::string& module,
                                    const std::string& path)
  {
    impl_->registerModulePath(module, path);
  }

  bool Debugger::waitForConfiguration(int timeoutMs)
  {
    return impl_->waitForConfiguration(timeoutMs);
  }

  void Debugger::notifyExecutionEnded()
  {
    impl_->notifyExecutionEnded();
  }

  void Debugger::setStopOnEntry(bool stopOnEntry)
  {
    impl_->setStopOnEntry(stopOnEntry);
  }

  void Debugger::writeOutput(const std::string& text)
  {
    impl_->writeOutput(text);
  }

  // ---------------------------------------------------------------------------
  // Impl: lifecycle
  // ---------------------------------------------------------------------------

  bool Debugger::Impl::attach(WrenVM* vm, int port)
  {
    if (!connection_.listen(port)) return false;

    // Sends happen over a socket the client may drop at any moment; a dead
    // peer must not take the host application down with SIGPIPE.
    signal(SIGPIPE, SIG_IGN);

    vm_ = vm;
    wrenSetDebugHook(vm, hookThunk, this);
    connection_.start([this](Json message) { handleMessage(std::move(message)); });
    return true;
  }

  void Debugger::Impl::detach()
  {
    if (vm_ != nullptr)
    {
      wrenSetDebugHook(vm_, nullptr, nullptr);
      vm_ = nullptr;
    }

    {
      std::unique_lock<std::mutex> lock(mutex_);
      paused_ = false;
      configurationDone_ = true;
      cv_.notify_all();
      configurationCv_.notify_all();
    }

    connection_.stop();
  }

  void Debugger::Impl::registerModulePath(const std::string& module,
                                          const std::string& path)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    moduleToPath_[module] = path;
    pathToModule_[path] = module;
  }

  bool Debugger::Impl::waitForConfiguration(int timeoutMs)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    stopOnEntry_ = true;

    if (timeoutMs < 0)
    {
      configurationCv_.wait(lock, [this] { return configurationDone_; });
      return configurationDone_;
    }

    bool configured = configurationCv_.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [this] { return configurationDone_; });
    return configured && configurationDone_;
  }

  void Debugger::Impl::notifyExecutionEnded()
  {
    if (!connection_.isConnected()) return;

    Json exited = Json::object();
    exited.set("exitCode", Json::integer(0));
    sendEvent("exited", std::move(exited));
    sendEvent("terminated", Json::object());
  }

  void Debugger::Impl::setStopOnEntry(bool stopOnEntry)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    stopOnEntry_ = stopOnEntry;
  }

  void Debugger::Impl::writeOutput(const std::string& text)
  {
    if (!connection_.isConnected()) return;

    Json body = Json::object();
    body.set("category", Json::string("stdout"));
    body.set("output", Json::string(text));
    sendEvent("output", std::move(body));
  }

  // ---------------------------------------------------------------------------
  // VM thread: the debug hook
  // ---------------------------------------------------------------------------

  void Debugger::Impl::hookThunk(WrenVM* vm, WrenDebugEvent event,
                                 void* userData)
  {
    static_cast<Impl*>(userData)->onLineEvent();
  }

  void Debugger::Impl::onLineEvent()
  {
    // Decide whether to stop on this line. The decision reads and updates
    // state guarded by [mutex_]; the VM's own state is stable because the
    // hook runs inside the interpreter loop.
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (vm_ == nullptr || !connection_.isConnected()) return;

      WrenDebugFrameInfo info;
      wrenDebugGetFrameInfo(vm_, 0, &info);
      int frameCount = wrenDebugGetFrameCount(vm_);

      const char* reason = nullptr;
      if (stopOnEntry_)
      {
        reason = "entry";
        stopOnEntry_ = false;
      }
      else if (isBreakpointAt(info))
      {
        reason = "breakpoint";
      }
      else if (pauseRequested_)
      {
        reason = "pause";
        pauseRequested_ = false;
      }
      else if (stepMode_ == StepMode::In)
      {
        reason = "step";
      }
      else if (stepMode_ == StepMode::Over && frameCount <= stepFromFrameCount_)
      {
        reason = "step";
      }
      else if (stepMode_ == StepMode::Out && frameCount < stepFromFrameCount_)
      {
        reason = "step";
      }

      if (reason == nullptr) return;

      // A step stops as soon as it has fired once.
      stepMode_ = StepMode::None;
      stopReason_ = reason;
    }

    pauseLoop();
  }

  bool Debugger::Impl::isBreakpointAt(const WrenDebugFrameInfo& info)
  {
    auto moduleBreakpoints = breakpoints_.find(info.module);
    if (moduleBreakpoints == breakpoints_.end()) return false;
    return moduleBreakpoints->second.count(info.line) > 0;
  }

  // Blocks the VM thread while the client inspects the paused program. DAP
  // requests that need live VM state are dequeued and answered here; the
  // loop ends when the client resumes execution.
  void Debugger::Impl::pauseLoop()
  {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      paused_ = true;
      clearVariableRefs();
    }

    Json body = Json::object();
    body.set("reason", Json::string(stopReason_));
    body.set("threadId", Json::integer(1));
    body.set("allThreadsStopped", Json::boolean(true));
    sendEvent("stopped", std::move(body));

    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
      if (!paused_) break;

      if (vmRequests_.empty())
      {
        cv_.wait(lock);
        continue;
      }

      VmRequest request = std::move(vmRequests_.front());
      vmRequests_.pop_front();
      lock.unlock();
      runVmRequest(request);
      lock.lock();
    }
  }

  void Debugger::Impl::runVmRequest(const VmRequest& request)
  {
    const std::string command = request.request.getString("command");
    const Json* arguments = request.request.find("arguments");
    const Json emptyArguments = Json::object();
    if (arguments == nullptr) arguments = &emptyArguments;

    Json body;
    if (command == "stackTrace")
    {
      body = buildStackFrames();
    }
    else if (command == "scopes")
    {
      body = buildScopes(arguments->getInt("frameId"));
    }
    else if (command == "variables")
    {
      body = buildVariables(arguments->getInt("variablesReference"));
    }
    else
    {
      sendErrorResponse(request.request,
                        "'" + command + "' requires the VM to be paused");
      return;
    }

    sendResponse(request.request, std::move(body));
  }

  // ---------------------------------------------------------------------------
  // Paused-state queries (VM thread, VM suspended in the hook)
  // ---------------------------------------------------------------------------

  Json Debugger::Impl::buildStackFrames()
  {
    Json frames = Json::array();

    int frameCount = wrenDebugGetFrameCount(vm_);
    for (int i = 0; i < frameCount; i++)
    {
      WrenDebugFrameInfo info;
      if (!wrenDebugGetFrameInfo(vm_, i, &info)) break;

      Json source = Json::object();
      std::string path;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        auto found = moduleToPath_.find(info.module);
        if (found != moduleToPath_.end()) path = found->second;
      }

      if (!path.empty())
      {
        source.set("name", Json::string(basename(path)));
        source.set("path", Json::string(path));
      }
      else
      {
        // Unmapped module: expose a synthetic source the client can fetch
        // with a "source" request.
        int reference = 0;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          for (const auto& entry : sourceReferences_)
          {
            if (entry.second == info.module) reference = entry.first;
          }
          if (reference == 0)
          {
            reference = nextSourceReference_++;
            sourceReferences_[reference] = info.module;
          }
        }
        source.set("name", Json::string(info.module));
        source.set("sourceReference", Json::integer(reference));
      }

      Json frame = Json::object();
      frame.set("id", Json::integer(i));
      frame.set("name", Json::string(info.function));
      frame.set("source", std::move(source));
      frame.set("line", Json::integer(info.line));
      frame.set("column", Json::integer(1));
      frames.append(std::move(frame));
    }

    Json body = Json::object();
    body.set("stackFrames", std::move(frames));
    body.set("totalFrames", Json::integer(frameCount));
    return body;
  }

  int Debugger::Impl::makeVariableRef(VariableRef::Kind kind, int frame,
                                      WrenHandle* value)
  {
    int reference = nextVariableRef_++;
    VariableRef& ref = variableRefs_[reference];
    ref.kind = kind;
    ref.frame = frame;
    ref.value = value;
    return reference;
  }

  void Debugger::Impl::clearVariableRefs()
  {
    for (auto& entry : variableRefs_)
    {
      if (entry.second.value != nullptr)
      {
        wrenReleaseHandle(vm_, entry.second.value);
      }
    }
    variableRefs_.clear();
    sourceReferences_.clear();
  }

  Json Debugger::Impl::buildScopes(int frame)
  {
    Json scopes = Json::array();

    // The frame's stack slots: receiver, parameters, and locals. Local
    // variable names are not retained at runtime, so slots are shown by
    // position.
    {
      int reference = makeVariableRef(VariableRef::Kind::Locals, frame, nullptr);
      Json scope = Json::object();
      scope.set("name", Json::string("Locals"));
      scope.set("presentationHint", Json::string("locals"));
      scope.set("variablesReference", Json::integer(reference));
      scope.set("expensive", Json::boolean(false));
      scopes.append(std::move(scope));
    }

    // The module's top-level variables, which do have names.
    int variableCount = wrenDebugGetModuleVariableCount(vm_, frame);
    if (variableCount > 0)
    {
      int reference = makeVariableRef(VariableRef::Kind::ModuleVariables,
                                      frame, nullptr);
      Json scope = Json::object();
      WrenDebugFrameInfo info;
      wrenDebugGetFrameInfo(vm_, frame, &info);
      scope.set("name", Json::string("Module: " + std::string(info.module)));
      scope.set("variablesReference", Json::integer(reference));
      scope.set("expensive", Json::boolean(false));
      scopes.append(std::move(scope));
    }

    Json body = Json::object();
    body.set("scopes", std::move(scopes));
    return body;
  }

  Json Debugger::Impl::buildVariables(int reference)
  {
    Json variables = Json::array();
    wrenEnsureSlots(vm_, 4);

    const VariableRef* ref = nullptr;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      auto found = variableRefs_.find(reference);
      if (found != variableRefs_.end()) ref = &found->second;
    }

    if (ref != nullptr)
    {
      // Appends one variable entry. Expandable values get their own
      // variablesReference so the client can fetch their children lazily.
      // (std::map references stay valid across insertion, so [ref] remains
      // usable even though appendVariable may add child references.)
      auto appendVariable = [&](const std::string& name, Value value)
      {
        Json variable = Json::object();
        variable.set("name", Json::string(name));
        variable.set("value", Json::string(formatValue(value)));
        if (isExpandable(value))
        {
          vm_->apiStack[2] = value;
          WrenHandle* handle = wrenGetSlotHandle(vm_, 2);
          VariableRef::Kind kind = VariableRef::Kind::List;
          ObjType type = AS_OBJ(value)->type;
          if (type == OBJ_MAP) kind = VariableRef::Kind::Map;
          else if (type == OBJ_INSTANCE) kind = VariableRef::Kind::Instance;
          int childReference = makeVariableRef(kind, ref->frame, handle);
          variable.set("variablesReference", Json::integer(childReference));
        }
        else
        {
          variable.set("variablesReference", Json::integer(0));
        }
        variables.append(std::move(variable));
      };

      switch (ref->kind)
      {
        case VariableRef::Kind::Locals:
        {
          // Local variable names are not retained at runtime. Slot 0 holds
          // the receiver, so present it as "this" in methods.
          WrenDebugFrameInfo info;
          wrenDebugGetFrameInfo(vm_, ref->frame, &info);
          bool isMethod = std::string(info.function) != "(script)";

          int count = wrenDebugGetLocalCount(vm_, ref->frame);
          for (int i = 0; i < count; i++)
          {
            wrenDebugGetLocal(vm_, ref->frame, i, 3);
            std::string name = (i == 0 && isMethod)
                ? "this" : "slot" + std::to_string(i);
            appendVariable(name, vm_->apiStack[3]);
          }
          break;
        }

        case VariableRef::Kind::ModuleVariables:
        {
          int count = wrenDebugGetModuleVariableCount(vm_, ref->frame);
          for (int i = 0; i < count; i++)
          {
            const char* name = wrenDebugGetModuleVariableName(vm_, ref->frame, i);
            wrenDebugGetModuleVariable(vm_, ref->frame, i, 3);
            appendVariable(name, vm_->apiStack[3]);
          }
          break;
        }

        case VariableRef::Kind::List:
        {
          setSlotFromRef(*ref, 0);
          int count = wrenGetListCount(vm_, 0);
          for (int i = 0; i < count; i++)
          {
            wrenGetListElement(vm_, 0, i, 3);
            appendVariable("[" + std::to_string(i) + "]", vm_->apiStack[3]);
          }
          break;
        }

        case VariableRef::Kind::Map:
        {
          setSlotFromRef(*ref, 0);
          ObjMap* map = AS_MAP(vm_->apiStack[0]);
          for (uint32_t i = 0; i < map->capacity; i++)
          {
            MapEntry* entry = &map->entries[i];
            if (IS_UNDEFINED(entry->key)) continue;
            appendVariable(truncate(formatValue(entry->key), 40),
                           entry->value);
          }
          break;
        }

        case VariableRef::Kind::Instance:
        {
          setSlotFromRef(*ref, 0);
          Obj* obj = AS_OBJ(vm_->apiStack[0]);
          ObjInstance* instance = (ObjInstance*)obj;
          for (int i = 0; i < obj->classObj->numFields; i++)
          {
            appendVariable("field" + std::to_string(i), instance->fields[i]);
          }
          break;
        }
      }
    }

    Json body = Json::object();
    body.set("variables", std::move(variables));
    return body;
  }

  void Debugger::Impl::setSlotFromRef(const VariableRef& ref, int slot)
  {
    wrenEnsureSlots(vm_, slot + 1);
    wrenSetSlotHandle(vm_, slot, ref.value);
  }

  // ---------------------------------------------------------------------------
  // DAP thread: request handling
  // ---------------------------------------------------------------------------

  void Debugger::Impl::handleMessage(Json message)
  {
    if (message.getString("type") != "request") return;
    const std::string command = message.getString("command");
    const Json* arguments = message.find("arguments");
    const Json emptyArguments = Json::object();
    if (arguments == nullptr) arguments = &emptyArguments;

    // Requests that describe what the client wants, answered from DAP state
    // alone.
    if (command == "initialize")
    {
      Json capabilities = Json::object();
      capabilities.set("supportsConfigurationDoneRequest", Json::boolean(true));
      capabilities.set("supportsEvaluateForHovers", Json::boolean(false));
      capabilities.set("supportsConditionalBreakpoints", Json::boolean(false));
      capabilities.set("supportTerminateDebuggee", Json::boolean(false));
      capabilities.set("supportsSetVariable", Json::boolean(false));
      sendResponse(message, std::move(capabilities));
      sendEvent("initialized", Json::object());
      return;
    }

    if (command == "launch" || command == "attach")
    {
      // Clients can pass "stopOnEntry": true/false in their launch
      // configuration to override the default behavior.
      if (arguments->find("stopOnEntry") != nullptr)
      {
        setStopOnEntry(arguments->getBool("stopOnEntry"));
      }
      sendResponse(message, Json::object());
      return;
    }

    if (command == "setBreakpoints")
    {
      const Json* source = arguments->find("source");
      std::string path = source != nullptr ? source->getString("path") : "";

      std::string module = moduleForPath(path);
      std::set<int> lines;
      const Json* requested = arguments->find("lines");
      if (requested != nullptr)
      {
        for (const Json& line : requested->arrayValue())
        {
          if (line.isNumber()) lines.insert(static_cast<int>(line.numberValue()));
        }
      }

      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (module.empty())
        {
          breakpoints_.erase(path);
        }
        else
        {
          breakpoints_[module] = lines;
        }
      }

      // Line-based breakpoints are checked at runtime, so report every
      // requested breakpoint as placed.
      Json placed = Json::array();
      const Json* clientBreakpoints = arguments->find("breakpoints");
      if (clientBreakpoints != nullptr)
      {
        for (const Json& breakpoint : clientBreakpoints->arrayValue())
        {
          Json entry = Json::object();
          entry.set("verified", Json::boolean(!module.empty()));
          entry.set("line", Json::integer(breakpoint.getInt("line")));
          placed.append(std::move(entry));
        }
      }

      Json body = Json::object();
      body.set("breakpoints", std::move(placed));
      sendResponse(message, std::move(body));
      return;
    }

    if (command == "setExceptionBreakpoints")
    {
      Json body = Json::object();
      body.set("breakpoints", Json::array());
      sendResponse(message, std::move(body));
      return;
    }

    if (command == "configurationDone")
    {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        configurationDone_ = true;
      }
      configurationCv_.notify_all();
      sendResponse(message, Json::object());
      return;
    }

    if (command == "threads")
    {
      Json thread = Json::object();
      thread.set("id", Json::integer(1));
      thread.set("name", Json::string("wren"));
      Json threads = Json::array();
      threads.append(std::move(thread));

      Json body = Json::object();
      body.set("threads", std::move(threads));
      sendResponse(message, std::move(body));
      return;
    }

    if (command == "source")
    {
      const Json* source = arguments->find("source");
      int reference = source != nullptr
          ? source->getInt("sourceReference") : 0;

      std::string module;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        auto found = sourceReferences_.find(reference);
        if (found != sourceReferences_.end()) module = found->second;
      }

      Json body = Json::object();
      body.set("content", Json::string(
          "// Module '" + module + "' has no registered source file.\n"
          "// The host can register one with Debugger::registerModulePath()."));
      sendResponse(message, std::move(body));
      return;
    }

    // Execution control.
    if (command == "continue")
    {
      resume(StepMode::None);
      Json body = Json::object();
      body.set("allThreadsContinued", Json::boolean(true));
      sendResponse(message, std::move(body));
      return;
    }

    if (command == "next" || command == "stepIn" || command == "stepOut")
    {
      StepMode mode = command == "next" ? StepMode::Over
          : command == "stepIn" ? StepMode::In : StepMode::Out;
      resume(mode);
      sendResponse(message, Json::object());
      return;
    }

    if (command == "pause")
    {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        pauseRequested_ = true;
      }
      sendResponse(message, Json::object());
      return;
    }

    if (command == "disconnect" || command == "terminate")
    {
      sendResponse(message, Json::object());

      // Resume the VM if it is paused so the host application doesn't stay
      // frozen, then drop the client so the server can accept the next one.
      {
        std::unique_lock<std::mutex> lock(mutex_);
        paused_ = false;
        cv_.notify_all();
      }
      connection_.disconnectClient();
      return;
    }

    // Requests that inspect live paused VM state are handed to the VM
    // thread's service loop.
    if (command == "stackTrace" || command == "scopes" ||
        command == "variables")
    {
      bool wasQueued = false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (paused_)
        {
          vmRequests_.push_back(VmRequest { std::move(message) });
          cv_.notify_all();
          wasQueued = true;
        }
      }
      if (!wasQueued)
      {
        sendErrorResponse(message, "Cannot inspect the VM while it is running");
      }
      // The response is sent by the VM thread once it processes the request.
      return;
    }

    if (command == "evaluate")
    {
      sendErrorResponse(message,
          "Expression evaluation is not supported by this debugger");
      return;
    }

    sendErrorResponse(message, "Unknown command '" + command + "'");
  }

  std::string Debugger::Impl::moduleForPath(const std::string& path)
  {
    std::unique_lock<std::mutex> lock(mutex_);

    auto found = pathToModule_.find(path);
    if (found != pathToModule_.end()) return found->second;

    // Fall back to matching by file name, which absorbs differences between
    // relative and absolute spellings of the same script.
    std::string name = basename(path);
    for (const auto& entry : moduleToPath_)
    {
      if (basename(entry.second) == name) return entry.first;
    }
    return "";
  }

  void Debugger::Impl::resume(StepMode mode)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!paused_) return;

    // The VM is paused in the hook, so its frame count can be read directly
    // and will not change until we let it run again.
    stepFromFrameCount_ = wrenDebugGetFrameCount(vm_);
    stepMode_ = mode;
    paused_ = false;
    cv_.notify_all();
  }

  // ---------------------------------------------------------------------------
  // Message plumbing (either thread)
  // ---------------------------------------------------------------------------

  void Debugger::Impl::send(Json message)
  {
    message.set("seq", Json::integer(sequence_.fetch_add(1)));
    connection_.send(message);
  }

  void Debugger::Impl::sendResponse(const Json& request, Json body)
  {
    Json response = Json::object();
    response.set("type", Json::string("response"));
    response.set("request_seq", Json::integer(request.getInt("seq")));
    response.set("success", Json::boolean(true));
    response.set("command", Json::string(request.getString("command")));
    response.set("body", std::move(body));
    send(std::move(response));
  }

  void Debugger::Impl::sendErrorResponse(const Json& request,
                                         const std::string& message)
  {
    Json response = Json::object();
    response.set("type", Json::string("response"));
    response.set("request_seq", Json::integer(request.getInt("seq")));
    response.set("success", Json::boolean(false));
    response.set("command", Json::string(request.getString("command")));
    response.set("message", Json::string(message));
    send(std::move(response));
  }

  void Debugger::Impl::sendEvent(const std::string& event, Json body)
  {
    Json message = Json::object();
    message.set("type", Json::string("event"));
    message.set("event", Json::string(event));
    message.set("body", std::move(body));
    send(std::move(message));
  }
}
}
