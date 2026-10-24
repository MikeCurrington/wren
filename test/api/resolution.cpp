#include <stdio.h>
#include <string.h>

#include "resolution.h"
#include "wren_vm.h"

// A test VM that prints to stdout and serves canned module sources.
struct ResolutionVM : WrenVM
{
  using WrenVM::WrenVM;

  void write(const char* text) override
  {
    printf("%s", text);
  }

  void error(WrenErrorType type, const char* module, int line,
             const char* message) override
  {
    if (type == WREN_ERROR_RUNTIME) printf("%s\n", message);
  }

  std::string loadModule(std::string_view module) override
  {
    printf("loading %s\n", std::string(module).c_str());

    if (module == "main/baz/bang")
    {
      return "import \"foo|bar\"";
    }
    return "System.print(\"ok\")";
  }
};

// Reports every import as a resolution error.
struct ReturnsNullVM : ResolutionVM
{
  using ResolutionVM::ResolutionVM;

  std::optional<std::string> resolveModule(std::string_view importer,
                                           std::string_view name) override
  {
    return std::nullopt;
  }
};

// Concatenates the importer and the import name, replacing "|" with "/" so
// the tests can use characters that are illegal in real paths.
struct ChangeVM : ResolutionVM
{
  using ResolutionVM::ResolutionVM;

  std::optional<std::string> resolveModule(std::string_view importer,
                                           std::string_view name) override
  {
    std::string result = std::string(importer) + "/" + std::string(name);
    for (char& c : result)
    {
      if (c == '|') c = '/';
    }
    return result;
  }
};

template <typename TestVM>
static void runTestVM(WrenVM* vm, const char* source)
{
  TestVM otherVM;

  // We should be able to execute code.
  WrenInterpretResult result = wrenInterpret(&otherVM, "main", source);
  if (result != WREN_RESULT_SUCCESS)
  {
    wrenSetSlotString(vm, 0, "error");
  }
  else
  {
    wrenSetSlotString(vm, 0, "success");
  }
}

static void noResolver(WrenVM* vm)
{
  // With no resolver override, the VM's default resolver passes logical
  // import strings along unchanged.
  runTestVM<ResolutionVM>(vm, "import \"foo/bar\"");
}

static void returnsNull(WrenVM* vm)
{
  runTestVM<ReturnsNullVM>(vm, "import \"foo/bar\"");
}

static void changesString(WrenVM* vm)
{
  runTestVM<ChangeVM>(vm, "import \"foo|bar\"");
}

static void shared(WrenVM* vm)
{
  runTestVM<ChangeVM>(vm, "import \"foo|bar\"\nimport \"foo/bar\"");
}

static void importer(WrenVM* vm)
{
  runTestVM<ChangeVM>(vm, "import \"baz|bang\"");
}

WrenForeignMethodFn resolutionBindMethod(const char* signature)
{
  if (strcmp(signature, "static Resolution.noResolver()") == 0) return noResolver;
  if (strcmp(signature, "static Resolution.returnsNull()") == 0) return returnsNull;
  if (strcmp(signature, "static Resolution.changesString()") == 0) return changesString;
  if (strcmp(signature, "static Resolution.shared()") == 0) return shared;
  if (strcmp(signature, "static Resolution.importer()") == 0) return importer;

  return NULL;
}

void resolutionBindClass(const char* className, WrenForeignClassMethods* methods)
{
//  methods->allocate = foreignClassAllocate;
}
