# Re-entrancy

This note describes how the VM supports calling out from Wren to C (foreign
methods and constructors) and then calling back into Wren from that C code,
recursively, without breaking fibers.

All of the scenarios below are implemented and tested in
`test/api/call_nested.wren`.

## wrenInterpret()

You can already call out to a foreign method or constructor from within an
execution that was started using `wrenInterpret()`, so I think that's fine.
`wrenInterpret()` doesn't use the API stack at all.

## wrenCall()

Normally, when using `wrenCall()` to start executing some code, the API slots
are at the very bottom of the fiber's stack and the fiber has no other
callframes until execution begins.

When a foreign method or constructor is called, there *are* callframes on the
fiber's stack. There must be, because that's where the arguments to the foreign
method are.

So, if you `wrenCall()`, which eventually calls a foreign method, the same fiber
will be used for the API twice. This works because `wrenCall()` clears
`vm->apiStack` as soon as it takes control, and `callForeign()` and
`createForeign()` save and restore the slot region themselves.

## Foreign calls

The interesting one is whether you can call `wrenInterpret()` or `wrenCall()`
from within a foreign method.

Calling `wrenInterpret()` works directly.

Calling `wrenCall()` requires a new set of API slots, because the slots that
are current inside a foreign method are the foreign method's own arguments.
The solution is a pair of functions:

    WrenCallContext* wrenBeginCall(WrenVM* vm);
    void wrenEndCall(WrenVM* vm, WrenCallContext* context);

`wrenBeginCall()` hides the current slots and creates a fresh fiber whose
stack forms a new, empty slot region. The host sets up the receiver and
arguments in those slots and calls `wrenCall()` as usual. When it's done, it
calls `wrenEndCall()`, which restores the previous (outer) slot region so the
foreign method can still read its arguments and write its return value.

The saved region is described by an *index* into the outer fiber's stack, not
a pointer, because stacks can be reallocated (and move) while nested code
runs. However, a fiber that is suspended in a foreign call can never be
resumed (see below), which means its stack can never grow while it is
suspended, which means the index remains valid.

The set of open contexts is tracked in `vm->callContexts`, a stack that
parallels the C call stack. If a foreign method returns without ending its
nested calls (an error, or simply a bug), `callForeign()` and `createForeign()`
clean them up.

## Nested foreign calls

Since each `wrenBeginCall()` creates a new fiber, you can nest arbitrarily:

    wrenCall()
    runInterpreter()
    callForeign()
    wrenBeginCall()
    wrenCall()
    runInterpreter()
    callForeign()
    ...

This preserves the invariant that any given Wren stack only ever has a single
foreign API call at the top of it.

The core `runInterpreter()` C function is itself re-entrant. Its cached state
(the current call frame, stack pointer, and instruction pointer) is stored
back into the fiber's frames with `STORE_FRAME()` before any code that can
re-enter the VM (foreign calls, in particular) and refreshed with
`LOAD_FRAME()` afterwards.

The garbage collector treats every fiber named by an open context as a root.
Those fibers are only reachable from the C stack, so without this they (and
all of the values on their stacks) would be collected while the nested code
runs.

## Calling re-entrant fibers

If Wren code gets a reference to a fiber that is suspended in a foreign call
(for example, by storing `Fiber.current` in a variable before the foreign call
and passing it into the nested code), calling or transferring to that fiber
would be catastrophic: it would run on a second interpreter while the first is
still waiting for the foreign method to return, and when it finished, the two
interpreters would unwind through each other.

To prevent this, `runFiber()` rejects calling or transferring to any fiber
named by an open context with a runtime error:

    Cannot call a fiber suspended in a foreign call.
    Cannot transfer to a fiber suspended in a foreign call.

(The pre-existing check that you cannot call the root fiber remains for the
ordinary, non-re-entrant case.)

## Suspending during re-entrancy

If the nested code suspends its fiber (using `Fiber.yield()` with no caller,
or by any other means), only the innermost interpreter stops. The nested
`wrenCall()` reports success, but there is no return value, and the suspended
fiber is no longer reachable from the VM, so it's up to the host to manage it.
In practice, hosts that need suspension should use their own fibers explicitly
rather than suspending nested calls.

A future API change may add a `WREN_RESULT_SUSPEND` case to
`WrenInterpretResult` so hosts can distinguish suspension from completion.
