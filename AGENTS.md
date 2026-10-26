# AGENT GUIDE: wren

Development is being done on Mac running OSX 

## 1. Project Overview

Fork of wren-lang/wren: the compiler and VM for the Wren scripting language.
Wren is the scripting language used by pinscript to script pinball machine play mechanics.
This fork adds a source-level debugger: Wren scripts, including those inside an
embedding application, can be debugged from VS Code over the Debug Adapter
Protocol (see doc/debugger.md).
Current work on branch feature/cpp converts the codebase from C to C++20.

## 2. Key technologies

Cmake for build configuration.
Active build project is 'projects/cmake'; './build_cmake.sh' configures, builds and runs tests.
C++ source code is in 'src' (vm, optional, debugger). Use C++20.
Public embedding API header is in 'src/include'.
Test scripts and suites are in 'test'; run via 'make test' in projects/cmake/build.

## 3. Boundaries & System Constraints

Never edit 'src/vm/wren_core.wren.inc'; it is generated from 'src/vm/wren_core.wren' by 'util/wren_to_c_string.py'.
Never modify files in 'build' or 'bin' folders; they are build output.
The other build systems in 'projects' (make, premake, xcode, vs2017, vs2019) are legacy; 'projects/cmake' is the active one.

## 4. Source control

Project uses git
Any commit messages you generate must end with the coding model name in parenthesis eg (GLM-5.3).
Be terse with commit messages, communicate intent of the change over implementation.

## 5. Model feedback & knowledge

Learnings discovered while working on this codebase (gotchas, conventions,
user feedback) are recorded here as they are found.
Ask the user for approval before adding an entry; never add unapproved.
Keep entries terse, one per line, newest last.

### Entries

- WrenVM member functions are PascalCase (Interpret(), SlotType()); host/test overrides match.
- VM implementation lives in WrenVM members; wren.h exports are one-line forwarders to them; free functions taking WrenVM* exist only where the embedding ABI or a function-pointer signature requires it.
- Interpreter state fields are private; non-member access goes through named public doors (Fiber/SetFiber, ApiStack/SetApiStack, Modules, SetCompiler, EnsureMethodName, ChargeBytes) or friends (Obj structs, WrenHandle, WrenCallContext, sCompiler).
- Core primitives are static WrenVM members named Prim<PascalCase>, declared from wren_core_prims.inc via the PRIM X-macro, defined and registered in wren_core.cpp.
