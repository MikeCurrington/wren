#ifndef wren_utils_h
#define wren_utils_h

#include "wren.h"
#include "wren_common.h"

// Reusable data structures and other utility functions.

// Forward declaration.
struct ObjString;

// Returns the smallest power of two that is equal to or greater than [n].
// Declared early here because Buffer<T>::fill() below relies on it.
int wrenPowerOf2Ceil(int n);

// We need buffers of a few different types. A template provides the same
// type-specific instances that the old preprocessor macros generated, but
// with type safety and ordinary method call syntax.
//
// Note: Wren heap objects are raw-allocated (see ALLOCATE in wren_common.h),
// so a Buffer embedded in one is never constructed. Call init() explicitly
// right after allocation, just like the old wrenXBufferInit() functions did.
template <typename T>
struct Buffer
{
  T* data;
  int count;
  int capacity;

  // Initializes the buffer to be empty.
  void init()
  {
    data = nullptr;
    capacity = 0;
    count = 0;
  }

  // Clears the buffer, deallocating any data used by it. Use this to
  // free a buffer when it's no longer needed.
  void clear(WrenVM* vm)
  {
    wrenReallocate(vm, data, 0, 0);
    init();
  }

  // Appends [count] copies of [value] to the buffer, growing it if needed.
  void fill(WrenVM* vm, T value, int count)
  {
    if (capacity < this->count + count)
    {
      int newCapacity = wrenPowerOf2Ceil(this->count + count);
      data = (T*)wrenReallocate(vm, data, capacity * sizeof(T),
                                newCapacity * sizeof(T));
      capacity = newCapacity;
    }

    for (int i = 0; i < count; i++)
    {
      data[this->count++] = value;
    }
  }

  // Appends [value] to the buffer, growing it if needed.
  void write(WrenVM* vm, T value)
  {
    fill(vm, value, 1);
  }
};

using ByteBuffer   = Buffer<uint8_t>;
using IntBuffer    = Buffer<int>;
using StringBuffer = Buffer<ObjString*>;

// TODO: Change this to use a map.
using SymbolTable = StringBuffer;

// Initializes the symbol table.
void wrenSymbolTableInit(SymbolTable* symbols);

// Frees all dynamically allocated memory used by the symbol table, but not the
// SymbolTable itself.
void wrenSymbolTableClear(WrenVM* vm, SymbolTable* symbols);

// Adds name to the symbol table. Returns the index of it in the table.
int wrenSymbolTableAdd(WrenVM* vm, SymbolTable* symbols,
                       const char* name, size_t length);

// Adds name to the symbol table. Returns the index of it in the table. Will
// use an existing symbol if already present.
int wrenSymbolTableEnsure(WrenVM* vm, SymbolTable* symbols,
                          const char* name, size_t length);

// Looks up name in the symbol table. Returns its index if found or -1 if not.
int wrenSymbolTableFind(const SymbolTable* symbols,
                        const char* name, size_t length);

void wrenBlackenSymbolTable(WrenVM* vm, SymbolTable* symbolTable);

// Returns the number of bytes needed to encode [value] in UTF-8.
//
// Returns 0 if [value] is too large to encode.
int wrenUtf8EncodeNumBytes(int value);

// Encodes value as a series of bytes in [bytes], which is assumed to be large
// enough to hold the encoded result.
//
// Returns the number of written bytes.
int wrenUtf8Encode(int value, uint8_t* bytes);

// Decodes the UTF-8 sequence starting at [bytes] (which has max [length]),
// returning the code point.
//
// Returns -1 if the bytes are not a valid UTF-8 sequence.
int wrenUtf8Decode(const uint8_t* bytes, uint32_t length);

// Returns the number of bytes in the UTF-8 sequence starting with [byte].
//
// If the character at that index is not the beginning of a UTF-8 sequence,
// returns 0.
int wrenUtf8DecodeNumBytes(uint8_t byte);


// Validates that [value] is within `[0, count)`. Also allows
// negative indices which map backwards from the end. Returns the valid positive
// index value. If invalid, returns `UINT32_MAX`.
uint32_t wrenValidateIndex(uint32_t count, int64_t value);

#endif
