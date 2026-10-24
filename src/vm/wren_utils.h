#ifndef wren_utils_h
#define wren_utils_h

#include "wren.h"
#include "wren_common.h"
#include <vector>

// Reusable data structures and other utility functions.

// Forward declaration.
struct ObjString;

// Returns the smallest power of two that is equal to or greater than [n].
// Declared early here because Buffer<T>::fill() below relies on it.
int wrenPowerOf2Ceil(int n);

#if 1

template <typename T>
using Buffer       = std::vector<T>;

using ByteBuffer   = Buffer<uint8_t>;
using IntBuffer    = Buffer<int>;
using StringBuffer = Buffer<ObjString*>;

#else 

// We need buffers of a few different types. A template provides the same
// type-specific instances that the old preprocessor macros generated, but
// with type safety and ordinary method call syntax.
//
// Heap objects created with wrenConstruct() are constructed, so a Buffer
// embedded in one initializes itself. For buffers living in memory that is
// allocated but never constructed (inside the parser and the compiler, which
// are plain stack structs), call init() explicitly, just like the old
// wrenXBufferInit() functions did.
//
// Constraints on T, the element type:
//
// - T must be trivially copyable. The buffer grows using the VM's
//   reallocate function, which moves the existing bytes to the new
//   allocation without running any constructors or destructors.
//
// The buffer itself:
//
// - Is not copyable or movable: it owns VM-allocated memory, and a copy
//   would free it twice when both buffers were cleared.
//
// - Has no destructor, by design. Heap objects that contain buffers are
//   never destroyed; their memory is released through clear() (usually via
//   the garbage collector's wrenFreeObj()). A destructor would either never
//   run or double-free.
template <typename T>
class Buffer
{
public:
  // Constructs an empty buffer.
  Buffer()
  {
    init();
  }

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&&) = delete;
  Buffer& operator=(Buffer&&) = delete;

  // Initializes the buffer to be empty. Only needed for buffers in memory
  // that was never constructed (see above).
  void init()
  {
    data_ = nullptr;
    capacity_ = 0;
    count_ = 0;
  }

  // Clears the buffer, deallocating any data used by it. Use this to
  // free a buffer when it's no longer needed.
  void clear(WrenVM* vm)
  {
    wrenReallocate(vm, data_, 0, 0);
    init();
  }

  // Appends [count] copies of [value] to the buffer, growing it if needed.
  void fill(WrenVM* vm, T value, int count)
  {
    if (capacity_ < count_ + count)
    {
      int newCapacity = wrenPowerOf2Ceil(count_ + count);
      data_ = (T*)wrenReallocate(vm, data_, capacity_ * sizeof(T),
                                 newCapacity * sizeof(T));
      capacity_ = newCapacity;
    }

    for (int i = 0; i < count; i++)
    {
      data_[count_++] = value;
    }
  }

  // Appends [value] to the buffer, growing it if needed.
  void write(WrenVM* vm, T value)
  {
    fill(vm, value, 1);
  }

  // Takes ownership of an already-allocated array of [count] elements with
  // room for [capacity]. The array must have come from the VM's allocator.
  // Used by constructors that allocate the array themselves.
  void adopt(T* data, int count, int capacity)
  {
    data_ = data;
    count_ = count;
    capacity_ = capacity;
  }

  // Drops the last element. The caller is responsible for having shifted or
  // saved whatever it contained first.
  void pop()
  {
    count_--;
  }

  // Shrinks the allocated storage to [newCapacity], which must be less than
  // or equal to capacity(). The elements themselves are not touched.
  void shrink(WrenVM* vm, int newCapacity)
  {
    data_ = (T*)wrenReallocate(vm, data_, capacity_ * sizeof(T),
                               newCapacity * sizeof(T));
    capacity_ = newCapacity;
  }

  // The number of elements currently in the buffer.
  int count() const { return count_; }

  // The number of elements the buffer can hold without growing.
  int capacity() const { return capacity_; }

  // Direct access to the element storage. The pointer is invalidated by any
  // fill() or write() that grows the buffer.
  T* data() { return data_; }
  const T* data() const { return data_; }

  T& operator[](int index) { return data_[index]; }
  const T& operator[](int index) const { return data_[index]; }

  bool empty() const { return count_ == 0; }

private:
  T* data_;
  int count_;
  int capacity_;
};

#endif // 0

// The table of interned method and variable names. Symbols are indexes into
// this table, so method dispatch is a direct array lookup.
//
// The same constraints as Buffer apply: not copyable, no destructor (the
// owning object is never destroyed), and call init() explicitly for tables
// living in memory that was never constructed.
class SymbolTable
{
public:
  SymbolTable() = default;

  SymbolTable(const SymbolTable&) = delete;
  SymbolTable& operator=(const SymbolTable&) = delete;
  SymbolTable(SymbolTable&&) = delete;
  SymbolTable& operator=(SymbolTable&&) = delete;

  // Initializes the table to be empty. Only needed for tables in memory
  // that was never constructed (inside the compiler): placement-constructs
  // the table in that memory.
  void init()
  {
#if 1
    new (&buffer_) StringBuffer();
#else
    buffer_.init();
#endif
  }

  // Frees all dynamically allocated memory used by the table.
  void clear(WrenVM* vm)
  {
#if 1
    // Assigning an empty vector releases the old storage; clear() alone
    // would keep the capacity allocated.
    buffer_ = {};
#else
    buffer_.clear(vm);
#endif
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
#if 1
  int count() const { return buffer_.size(); }
#else
  int count() const { return buffer_.count(); }
#endif

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
