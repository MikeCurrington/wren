#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>

#include "wren_vm.h"
#include "wren_debugger.h"

// A minimal Wren host that makes its scripts debuggable from VS Code.
//
//   wren_debug_example [--port N] [--wait-ms N] script.wren
//
// Listens on 127.0.0.1:N (default 4711) for a Debug Adapter Protocol client.
// With --wait-ms, it waits up to that long for the client to attach and send
// its breakpoints before running the script; the first line then stops with
// an "entry" stop.

namespace
{
  struct HostState
  {
    wren::debug::Debugger* debugger;
    std::string scriptDir;
  };

  std::string directoryOf(const std::string& path)
  {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return path.substr(0, slash);
  }

  // The host's VM: writes to stdout (mirrored to the debug client's console),
  // reports errors to stderr, and loads imported modules from files next to
  // the main script.
  struct HostVM : WrenVM
  {
    explicit HostVM(HostState* state) : state_(state) {}

    void Write(const char* text) override
    {
      fputs(text, stdout);
      fflush(stdout);
      state_->debugger->writeOutput(text);
    }

    void Error(WrenErrorType type, const char* module, int line,
               const char* message) override
    {
      if (type == WREN_ERROR_COMPILE)
      {
        fprintf(stderr, "[%s line %d] %s\n", module, line, message);
      }
      else if (type == WREN_ERROR_RUNTIME)
      {
        fprintf(stderr, "Error: %s\n", message);
      }
      else
      {
        fprintf(stderr, "[%s line %d] in %s\n", module, line, message);
      }
    }

    std::string LoadModule(std::string_view name) override
    {
      // Resolve [name] (a Wren import string) to a file next to the main
      // script: "util" or "./util" both resolve to <scriptDir>/util.wren.
      std::string module(name);
      if (module.rfind("./", 0) == 0) module = module.substr(2);
      std::string path = state_->scriptDir + "/" + module + ".wren";

      FILE* file = fopen(path.c_str(), "rb");
      if (file == nullptr) return std::string();

      fseek(file, 0, SEEK_END);
      long size = ftell(file);
      fseek(file, 0, SEEK_SET);

      std::string source(static_cast<size_t>(size), '\0');
      size_t read = fread(&source[0], 1, size, file);
      fclose(file);
      source.resize(read);

      state_->debugger->registerModulePath(std::string(name), path);
      return source;
    }

    HostState* state_;
  };

  void printUsage()
  {
    fprintf(stderr,
        "Usage: wren_debug_example [--port N] [--wait-ms N] script.wren\n");
  }
}

int main(int argc, const char* argv[])
{
  int port = 4711;
  int waitMs = -1;
  const char* scriptPath = nullptr;

  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
    {
      port = atoi(argv[++i]);
    }
    else if (strcmp(argv[i], "--wait-ms") == 0 && i + 1 < argc)
    {
      waitMs = atoi(argv[++i]);
    }
    else if (argv[i][0] != '-')
    {
      scriptPath = argv[i];
    }
    else
    {
      printUsage();
      return 1;
    }
  }

  if (scriptPath == nullptr)
  {
    printUsage();
    return 1;
  }

  FILE* file = fopen(scriptPath, "rb");
  if (file == nullptr)
  {
    fprintf(stderr, "Could not open script '%s'.\n", scriptPath);
    return 1;
  }
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  std::string source(static_cast<size_t>(size), '\0');
  size_t read = fread(&source[0], 1, size, file);
  fclose(file);
  source.resize(read);

  std::unique_ptr<wren::debug::Debugger> debugger = wren::debug::MakeDebugger();

  HostState state;
  state.debugger = debugger.get();
  state.scriptDir = directoryOf(scriptPath);

  HostVM vm(&state);

  if (!debugger->attach(&vm, port))
  {
    fprintf(stderr, "Could not listen on port %d.\n", port);
    return 1;
  }

  fprintf(stderr, "Debugger listening on 127.0.0.1:%d\n", port);

  // Register before waiting so breakpoints sent during initial configuration
  // can already be mapped from source paths to module names.
  debugger->registerModulePath(scriptPath, scriptPath);

  if (waitMs >= 0)
  {
    if (debugger->waitForConfiguration(waitMs))
    {
      fprintf(stderr, "Client attached; configuration complete.\n");
    }
    else
    {
      fprintf(stderr, "No client attached in time; running anyway.\n");
    }
  }

  WrenInterpretResult result = wrenInterpret(&vm, scriptPath, source.c_str());
  debugger->notifyExecutionEnded();

  return result == WREN_RESULT_SUCCESS ? 0 : 1;
}
