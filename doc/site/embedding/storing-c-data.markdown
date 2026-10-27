^title Storing C Data

An embedded language often needs to work with native data. You may want a
pointer to some memory managed in the C heap, or maybe you want to store a chunk
of data more efficiently than Wren's dynamism allows. You may want a Wren object
that represents a native resource like a file handle or database connection.

For those cases, you can define a **foreign class**, a chimera whose state is
half Wren and half C. It is a real Wren class with a name, constructor, and
methods. You can define methods on it written in Wren, or [foreign methods][]
written in C. It produces real Wren objects that you can pass around, do `is`
checks on, etc. But it also owns a pointer to some data in your host
application that is opaque to Wren but accessible from C.

[foreign methods]: calling-c-from-wren.html

## Defining a Foreign Class

You define one like so:

<pre class="snippet">
foreign class Point {
  // ...
}
</pre>

The `foreign` keyword tells Wren to loop in the host application when it
constructs instances of the class. The host hands Wren an object to own, and
Wren holds onto it as long as the foreign instance is reachable.

To talk to the host app, Wren needs a C function it can call when it constructs
an instance of the foreign class. This function is found through a binding
process similar to [how foreign methods are bound][bind]. When you [configure
the VM][], you set the `bindForeignClassFn` field in WrenConfiguration to point
to a C callback you define. Its signature must be:

[bind]: calling-c-from-wren.html#binding-foreign-methods
[configure the vm]: configuring-the-vm.html

<pre class="snippet" data-lang="c">
WrenForeignClassMethods bindForeignClass(
    WrenVM* vm, const char* module, const char* className);
</pre>

Wren invokes this callback once when a foreign class declaration is executed.
Wren passes in the name of the module containing the foreign class, and the name
of the class being declared. The host's responsibility is to return one of these
structs:

<pre class="snippet" data-lang="c">
typedef struct
{
  WrenForeignMethodFn allocate;
  WrenFinalizerFn finalize;
} WrenForeignClassMethods;
</pre>

It's a pair of function pointers. The first, `allocate`, is called by Wren
whenever an instance of the foreign class is created. (We'll get to the optional
`finalize` callback later.) The allocation callback has the same signature as a
foreign method:

<pre class="snippet" data-lang="c">
void allocate(WrenVM* vm);
</pre>

## Initializing an Instance

When you create an instance of a foreign class by calling one its
[constructors][], Wren invokes the `allocate` callback you gave it when binding
the foreign class. Your primary responsibility in that callback is to create
the native data and give it to Wren to own. You do that by calling:

[constructors]: ../classes.html#constructors

<pre class="snippet" data-lang="cpp">
void WrenVM::SetSlotNewForeignShared(int slot, int classSlot,
    std::shared_ptr&lt;void&gt; object, const std::type_info* type);
</pre>

Like the other slot methods, it both reads from and writes to the slot array.
Its parameters are:

[slot]: slots-and-handles.html

* The `slot` parameter is the destination slot where the new foreign object
  should be placed. When you're calling this in a foreign class's allocate
  callback, this should be 0.

* The `classSlot` parameter is the slot where the foreign class being
  constructed can be found. When the VM calls an allocate callback for a
  foreign class, the class itself is already in slot 0, so you'll pass 0 for
  this too.

* The `object` parameter is the interesting one. Here, you pass a
  `shared_ptr` to the native data the instance should own. It may point at
  anything your host defines; Wren treats it as opaque. Pass an empty
  `shared_ptr` if the instance carries no data.

* Finally, `type` records the payload's static type for the C++ layer's
  checked extraction. Pass `nullptr` if you don't need it.

Foreign objects are reference-counted: the VM holds one reference and releases
it when the garbage collector reclaims the instance, and your host can hold
its own references to keep the payload alive past collection.

So, for example, to create a foreign instance that owns a heap-allocated
`double`:

<pre class="snippet" data-lang="cpp">
vm->SetSlotNewForeignShared(0, 0,
    std::shared_ptr&lt;double&gt;(new double(1.5),
        std::default_delete&lt;double&gt;()), nullptr);
</pre>

Any parameters passed to the constructor are also available in subsequent slots
in the slot array. That way you can initialize the foreign data based on values
passed to the constructor from Wren.

After the allocate callback returns, the class's constructor in Wren is run and
execution proceeds like normal. From here on out, within Wren, it appears you
have a normal instance of a class. It just happens to hold a pointer to your
native data that can be accessed from foreign methods.

## Accessing Foreign Data

Typically, the way you make use of the data stored in an instance of a foreign
class is through other foreign methods. Those are usually defined on the same
foreign class, but can be defined on other classes as well. Wren doesn't care.

Once you have a foreign instance in a slot, you can access the data it owns by
calling:

<pre class="snippet" data-lang="cpp">
void* WrenVM::GetSlotForeign(int slot);
</pre>

You pass in the slot index containing the foreign object and it gives you back
the pointer you stored with `SetSlotNewForeignShared()`. The method doesn't do
any type checking, so it's on you to make sure the object in that slot
actually *is* an instance of the foreign class you expect and to cast the
pointer back to the right type.

Given that void pointer, you can now freely read and modify the data it points
to. They're your bits, Wren just holds them for you. If you want shared
ownership — say, to keep the payload alive after the foreign instance dies —
`WrenVM::GetSlotForeignShared(slot)` hands you back the payload's
`shared_ptr` itself.

## Freeing Resources

The payload you store is a `shared_ptr`, so its lifetime ends when its last
reference goes away: the garbage collector releases the VM's reference when
the foreign instance is reclaimed, and any `shared_ptr` your host kept
(copied from `GetSlotForeignShared()`) keeps the object alive until it too is
destroyed. If the payload's type has a destructor, it runs then — plain RAII.

But, often, your foreign data refers to some resource whose lifetime needs to
be explicitly managed. For example, if you have a foreign object that wraps an
open file handle, you need to ensure that handle doesn't get left open when the
GC frees the foreign instance.

Of course, you can (and usually should) add a method on your foreign class, like
`close()` so the user can explicitly release the resource managed by the object.
But if they forget to do that and the object is no longer reachable, you want to
make sure the resource isn't leaked. The payload's destructor covers that
case.

On top of the payload's destructor, you can also provide a *finalizer* function
when binding the foreign class. That's the other callback in the
WrenForeignClassMethods struct. If you provide that callback, then Wren will
invoke it when an instance of your foreign class is about to be freed by the
garbage collector. This gives you one last chance to observe the object as it
dies — usually to update counters or diagnostics.

Because this is called during the middle of a garbage collection, you do not
have unfettered access to the VM. It's not like a normal foreign method where
you can monkey around with slots and other stuff. Doing that while the GC is
running could leave Wren in a weird state.

Instead, the finalize callback's signature is only:

<pre class="snippet" data-lang="c">
void finalize(void* data);
</pre>

Wren gives you the pointer to your foreign object, and that's it. The *only*
thing you should do inside a finalizer is release any external resources
referenced by that object.

## A Full Example

That's a lot to take in, so let's walk through a full example of a foreign class
with a finalizer and a couple of methods. We'll do a File class that wraps the
C standard file API.

In Wren, the class we want looks like this:

<pre class="snippet">
foreign class File {
  construct create(path) {}

  foreign write(text)
  foreign close()
}
</pre>

So you can create a new file given a path. Once you have one, you can write to
it and then explicitly close it if you want. We also need to make sure the file
gets closed if the user forgets to and the GC cleans up the object.

### Setting up the VM

Over in the host, first we'll set up the VM:

<pre class="snippet" data-lang="c">
#include "wren.h"

int main(int argc, const char* argv[])
{
  WrenConfiguration config;
  wrenInitConfiguration(&config);

  config.bindForeignClassFn = bindForeignClass;
  config.bindForeignMethodFn = bindForeignMethod;

  WrenVM* vm = wrenNewVM(&config);
  wrenInterpret(vm, "my_module", "some code...");

  return 0;
}
</pre>

### Binding the foreign class

We give the VM two callbacks. The first is for wiring up the foreign class
itself:

<pre class="snippet" data-lang="c">
WrenForeignClassMethods bindForeignClass(
    WrenVM* vm, const char* module, const char* className)
{
  WrenForeignClassMethods methods;

  if (strcmp(className, "File") == 0)
  {
    methods.allocate = fileAllocate;
    methods.finalize = fileFinalize;
  }
  else
  {
    // Unknown class.
    methods.allocate = NULL;
    methods.finalize = NULL;
  }

  return methods;
}
</pre>

When our binding callback is invoked for the File class, we return the allocate
function the VM should call. (We leave `finalize` NULL; the payload's
destructor below handles cleanup.) Allocation looks like:

<pre class="snippet" data-lang="cpp">
#include &lt;stdio.h>
#include &lt;memory>
#include "wren_vm.h"

// A holder for the file handle. The foreign object owns one of these, and
// the holder owns the FILE*.
struct FileData
{
  FILE* file = NULL;

  ~FileData()
  {
    // Already closed.
    if (file == NULL) return;

    fclose(file);
    file = NULL;
  }
};

void fileAllocate(WrenVM* vm)
{
  const char* path = wrenGetSlotString(vm, 1);
  auto data = std::make_shared&lt;FileData&gt;();
  data-&gt;file = fopen(path, "w");
  vm->SetSlotNewForeignShared(0, 0, data, nullptr);
}
</pre>

First we pull the file path from the slot array. Then we create a `FileData`
holder for the file handle and tell C to create a new file at that path.
Finally we hand the holder to Wren with `SetSlotNewForeignShared()`. When the
instance is collected and no other `shared_ptr` references the holder, its
destructor runs, which closes the file (if it's not already closed) and nulls
out the file handle first so that we don't try to use the file after it's been
closed.

### Binding the foreign methods

That's the foreign *class* part. Now we have a couple of foreign *methods* to
handle. The host tells the VM how to find them by giving Wren a pointer to this
function:

<pre class="snippet" data-lang="c">
WrenForeignMethodFn bindForeignMethod(WrenVM* vm, const char* module,
    const char* className, bool isStatic, const char* signature)
{
  if (strcmp(className, "File") == 0)
  {
    if (!isStatic && strcmp(signature, "write(_)") == 0)
    {
      return fileWrite;
    }

    if (!isStatic && strcmp(signature, "close()") == 0)
    {
      return fileClose;
    }
  }

  // Unknown method.
  return NULL;
}
</pre>

When Wren calls this, we look at the class and method name to figure out which
method it's binding, and then return a pointer to the appropriate function. The
foreign method for writing to the file is:

<pre class="snippet" data-lang="cpp">
void fileWrite(WrenVM* vm)
{
  FileData* data = static_cast&lt;FileData*&gt;(vm->GetSlotForeign(0));

  // Make sure the file is still open.
  if (data-&gt;file == NULL)
  {
    wrenSetSlotString(vm, 0, "Cannot write to a closed file.");
    wrenAbortFiber(vm, 0);
    return;
  }

  const char* text = wrenGetSlotString(vm, 1);
  fwrite(text, sizeof(char), strlen(text), data-&gt;file);
}
</pre>

We use `GetSlotForeign()` to pull the foreign data out of the slot array.
Since this method is called on the file itself, the foreign object is in slot
zero. We take the resulting pointer and cast it back to the proper type.

We do a little sanity checking to make sure the user isn't writing to a file
they already closed. If not, we call `fwrite()` to write to the file.

The other method is `close()` to let them explicitly close the file:

<pre class="snippet" data-lang="cpp">
void fileClose(WrenVM* vm)
{
  FileData* data = static_cast&lt;FileData*&gt;(vm->GetSlotForeign(0));

  // Already closed.
  if (data-&gt;file == NULL) return;

  fclose(data-&gt;file);
  data-&gt;file = NULL;
}
</pre>

It uses the same helper we defined above. And that's it, a complete foreign
class with a finalizer and a couple of foreign methods. In Wren, you can use it
like so:

<pre class="snippet">
var file = File.create("some/path.txt")
file.write("some text")
file.close()
</pre>

Pretty neat, right? The resulting class looks and feels like a normal Wren
class, but it has the functionality and much of the performance of native C
code.

<a class="right" href="configuring-the-vm.html">Configuring the VM &rarr;</a>
<a href="calling-c-from-wren.html">&larr; Calling C from Wren</a>
