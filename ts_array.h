// Copyright (c) [2024] [Jovan J. E. Odassius]
//
// License: MIT (See the LICENSE file in the root directory)
// Github: https://github.com/untyper/thread-safe-array

#ifndef LOCKFREE_THREADSAFE_ARRAY
#define LOCKFREE_THREADSAFE_ARRAY

#include <atomic>
#include <memory>
#include <optional>
#include <functional>
#include <array>
#include <utility>
#include <cstddef>
// #include <iostream>

template <typename T, std::size_t Size>
class Array
{
private:
  struct Entry
  {
    std::shared_ptr<T> value{ nullptr };
    std::atomic<std::size_t> next_free_index{ 0 };
  };

  std::array<Entry, Size> data;
  std::atomic<uint64_t> free_list_head; // Stores index and counter
  static constexpr std::size_t INVALID_INDEX = Size;

  // Helper functions to pack and unpack index and counter
  uint64_t pack_index_counter(std::size_t index, std::size_t counter) const
  {
    return (static_cast<uint64_t>(counter) << 32) | index;
  }

  void unpack_index_counter(uint64_t value, std::size_t& index, std::size_t& counter) const
  {
    index = static_cast<std::size_t>(value & 0xFFFFFFFF);
    counter = static_cast<std::size_t>(value >> 32);
  }

  // Push an index onto the free list
  void push_free_index(std::size_t index)
  {
    uint64_t old_head_value = this->free_list_head.load(std::memory_order_relaxed);
    uint64_t new_head_value;
    std::size_t old_head_index, old_head_counter;

    do
    {
      unpack_index_counter(old_head_value, old_head_index, old_head_counter);

      // Set the next_free_index in the Entry to the old head index
      this->data[index].next_free_index.store(old_head_index, std::memory_order_relaxed);

      // Prepare new head value with the new index and incremented counter
      std::size_t new_counter = old_head_counter + 1;
      new_head_value = pack_index_counter(index, new_counter);
    } while (!this->free_list_head.compare_exchange_weak(
      old_head_value, new_head_value, std::memory_order_release, std::memory_order_relaxed));
  }

  // Pop an index from the free list
  bool pop_free_index(std::size_t& index)
  {
    uint64_t old_head_value = this->free_list_head.load(std::memory_order_relaxed);
    uint64_t new_head_value;
    std::size_t old_head_index, old_head_counter;

    do
    {
      unpack_index_counter(old_head_value, old_head_index, old_head_counter);

      if (old_head_index == INVALID_INDEX)
      {
        return false; // Free list is empty
      }

      index = old_head_index;

      // Load the next index from the Entry
      std::size_t next_index = this->data[index].next_free_index.load(std::memory_order_relaxed);

      // Prepare new head value with next_index and incremented counter
      std::size_t new_counter = old_head_counter + 1;
      new_head_value = pack_index_counter(next_index, new_counter);

      if (this->free_list_head.compare_exchange_weak(
        old_head_value, new_head_value, std::memory_order_acquire, std::memory_order_relaxed))
      {
        return true;
      }
    } while (true);
  }

public:
  // Add an element to the array using perfect forwarding
  template<typename... Args>
  std::optional<std::size_t> insert(Args&&... args)
  {
    std::size_t index;

    if (!this->pop_free_index(index))
    {
      return std::nullopt; // Array is full
    }

    // Create a shared_ptr<T> by perfectly forwarding the arguments
    std::shared_ptr<T> new_value = std::make_shared<T>(std::forward<Args>(args)...);

    // Try to set the value atomically
    std::shared_ptr<T> expected = nullptr;

    if (!std::atomic_compare_exchange_strong(
      &this->data[index].value,
      &expected,
      new_value))
    {
      // Failed to set the value; someone else may have set it
      // Do not push the index back into the free list, as it is now in use
      return std::nullopt;
    }

    return index;
  }

  // Find an element based on a predicate, returning its index
  template <typename Predicate>
  std::optional<std::size_t> find_if(Predicate predicate) const
  {
    for (std::size_t i = 0; i < Size; ++i)
    {
      std::shared_ptr<T> element = std::atomic_load(&this->data[i].value);

      if (element && predicate(*element))
      {
        return i;
      }
    }

    return std::nullopt; // Element not found
  }

  // Find an element directly by value, returning its index
  std::optional<std::size_t> find(const T& value) const
  {
    return this->find_if([&value](const T& element) { return element == value; });
  }

  // Remove an element based on its index
  bool erase(std::size_t index)
  {
    if (index >= Size)
    {
      return false; // Invalid index
    }

    std::shared_ptr<T> expected = std::atomic_load(&this->data[index].value);

    while (expected)
    {
      if (std::atomic_compare_exchange_weak(
          &this->data[index].value,
          &expected,
          std::shared_ptr<T>(nullptr)))
      {
        // The shared_ptr destructor will handle deletion when no references remain
        this->push_free_index(index);
        return true; // Successfully erased
      }
      // If compare_exchange_weak fails, expected is updated to the current value
    }

    return false; // Element was already null or erased by another thread
  }

  // Retrieve an element at the given index
  std::optional<T> at(std::size_t index) const
  {
    if (index >= Size)
    {
      return std::nullopt; // Invalid index
    }

    std::shared_ptr<T> element = std::atomic_load(&this->data[index].value);

    if (element)
    {
      return *element; // Return a copy of T
    }

    return std::nullopt; // Element is null
  }

  // Get the current size of the array
  std::size_t size() const
  {
    std::size_t count = 0;

    for (std::size_t i = 0; i < Size; ++i)
    {
      if (std::atomic_load(&this->data[i].value))
      {
        ++count;
      }
    }

    return count;
  }

  std::size_t capacity() const
  {
    return Size;
  }

  Array()
  {
    // Initialize the free list
    for (std::size_t i = 0; i < Size - 1; ++i)
    {
      this->data[i].next_free_index.store(i + 1, std::memory_order_relaxed);
    }

    this->data[Size - 1].next_free_index.store(INVALID_INDEX, std::memory_order_relaxed);

    // Initialize free_list_head with the initial index and counter 0
    this->free_list_head.store(pack_index_counter(0, 0), std::memory_order_relaxed);
  }
};

#endif // LOCKFREE_THREADSAFE_ARRAY
