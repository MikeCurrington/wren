#ifndef wren_utils_h
#define wren_utils_h

#include "wren.h"
#include "wren_common.h"
#include <vector>

// Reusable data structures and other utility functions.

// Forward declaration.
struct ObjString;

// Returns the smallest power of two that is equal to or greater than [n].
// Used to size fiber stacks and map entry arrays.
int wrenPowerOf2Ceil(int n);

template <typename T>
using Buffer       = std::vector<T>;

using ByteBuffer   = Buffer<uint8_t>;
using IntBuffer    = Buffer<int>;
using StringBuffer = Buffer<ObjString*>;

// The table of interned method and variable names. Symbols are indexes into
// this table, so method dispatch is a direct array lookup.
//
// Not copyable: a copy would intern the same names in two tables. Methods
// that allocate or touch the garbage collector take the [WrenVM] explicitly,
// like the rest of the VM's utility classes.
class SymbolTable
{
public:
  SymbolTable() = default;

  SymbolTable(const SymbolTable&) = delete;
  SymbolTable& operator=(const SymbolTable&) = delete;
  SymbolTable(SymbolTable&&) = delete;
  SymbolTable& operator=(SymbolTable&&) = delete;

  // Frees all dynamically allocated memory used by the table.
  void clear()
  {
    // Assigning an empty vector releases the old storage; clear() alone
    // would keep the capacity allocated.
    buffer_ = {};
  }

  // Adds name to the symbol table. Returns the index of it in the table.
  int add(WrenVM* vm, const char* name, size_t length);

  // Adds name to the symbol table. Returns the index of it in the table.
  // Will use an existing symbol if already present.
  int ensure(WrenVM* vm, const char* name, size_t length);

  // Looks up name in the symbol table. Returns its index if found or -1 if
  // not.
  int find(const char* name, size_t length) const;

  // The number of symbols in the table.
  int count() const { return buffer_.size(); }

  // The name of the symbol at [index].
  ObjString* operator[](int index) const { return buffer_[index]; }

  // Marks the symbols reachable for the garbage collector.
  void blacken(WrenVM* vm);

private:
  StringBuffer buffer_;
};

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
