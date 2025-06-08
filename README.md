# thread-safe-array
Lock-free thread-safe array for C++ 17 (and up)

# Safe_Array

A **lock-free**, **thread-safe**, fixed-capacity array that stores objects **in-place** (no per-element heap allocations) and supports concurrent `insert`, `erase` and read (via index, predicate, or iteration).

## Features

- **Fixed capacity** (`Capacity` is a compile-time template parameter)  
- **In-place storage**: constructs `T` directly in a byte buffer  
- **Lock-free**: all operations use only atomic CAS/load/store  
- **Thread-safe**: multiple threads may call `insert(...)`, `erase(...)`, or the read APIs simultaneously  
- **Simple iteration**: `for_each(...)` visits all live elements

## Requirements

- C++17  
- Headers: `<array>`, `<atomic>`, `<cstdint>`, `<cstddef>`, `<optional>`, `<type_traits>`, `<new>`, `<utility>`

## Public API

```cpp
template<typename T, std::size_t Capacity>
class Safe_Array
{
public:
  struct Op_Result
  {
    std::size_t index;
    T&          value;
  };

  Safe_Array();                             // default ctor
  ~Safe_Array();                            // destroys any remaining T

  // Attempts to construct T(args...) in a free slot.
  // On success returns { index, reference-to-T }, otherwise nullopt.
  template<typename... Args>
  std::optional<Op_Result> insert(Args&&... args);

  // Erase the element at `index`. Returns true if it was present.
  bool erase(std::size_t index);

  // Access by index if live
  std::optional<Op_Result> at(std::size_t index) const;

  // Find the first element matching predicate
  template<typename Predicate>
  std::optional<Op_Result> find_if(Predicate pred) const;

  // Find by equality
  std::optional<Op_Result> find(const T& value) const;

  // Number of live elements (O(Capacity))
  std::size_t size() const;

  // Compile-time capacity
  constexpr std::size_t capacity() const;

  // Call f(index, value) for each live element
  template<typename Func>
  void for_each(Func f) const;
};
```

## Examples

### Basic usage

```c++
#include "safe_array.h"
#include <iostream>

int main()
{
  Safe_Array<int, 8> arr;

  // Insert a few values
  if (auto r = arr.insert(42))
  {
    std::cout << "Inserted 42 at slot " << r->index << "\n";
  }

  arr.insert(7);
  arr.insert(13);

  // Iterate
  arr.for_each([](std::size_t i, int& v)
  {
    std::cout << i << " => " << v << "\n";
  });

  // Remove one
  arr.erase(1);  // true if slot 1 was READY

  std::cout << "Size: " << arr.size() << "\n";
}
```

### Multi-threaded producers

```c++
#include "safe_array.h"
#include <thread>
#include <vector>
#include <iostream>

int main()
{
  Safe_Array<int, 100> arr;

  // Launch 4 threads that insert numbers
  std::vector<std::thread> threads;

  for (int t = 0; t < 4; ++t)
  {
    threads.emplace_back([&arr, t]()
    {
      for (int i = 0; i < 25; ++i)
      {
        arr.insert(t * 100 + i);
      }
    });
  }

  for (auto& th : threads) th.join();

  // Single-threaded iterate & print
  arr.for_each([](std::size_t i, int& v)
  {
    std::cout << "[" << i << "] = " << v << "\n";
  });

  std::cout << "Total live: " << arr.size() << "\n";
}
```

## Notes
- Very basic lock-free thread-safe `Safe_Array` implementation
- Has not been tested extensively
- Order of elements is not guaranteed
- No external dependencies or platform specific code
