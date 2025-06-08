// Copyright (c) [2024] [Jovan J. E. Odassius]
//
// License: MIT (See the LICENSE file in the root directory)
// Github: https://github.com/untyper/thread-safe-array

#ifndef LOCKFREE_THREADSAFE_ARRAY
#define LOCKFREE_THREADSAFE_ARRAY

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <new>
#include <utility>
//#include <iostream>

template<typename T, std::size_t Capacity>
class Safe_Array
{
  static_assert(std::is_nothrow_destructible<T>::value,
    "T must be nothrow destructible");

private:
  struct Entry
  {
    // Low 2 bits = state; upper bits = ABA counter
    static constexpr std::uint32_t STATE_MASK = 0x3;
    enum SlotState : std::uint32_t
    {
      EMPTY = 0,
      INIT = 1,
      READY = 2,
      REMOVING = 3
    };

    std::atomic<std::uint32_t> state{ EMPTY };
    alignas(T) unsigned char storage[sizeof(T)];
    std::atomic<std::size_t> next_free_index{ 0 };
  };

  std::array<Entry, Capacity> data;
  std::atomic<std::uint64_t> free_list_head;
  static constexpr std::size_t INVALID_INDEX = Capacity;

  std::uint64_t pack_index_counter(std::size_t idx, std::size_t ctr) const
  {
    return (std::uint64_t(ctr) << 32) | idx;
  }

  void unpack_index_counter(std::uint64_t v, std::size_t& idx, std::size_t& ctr) const
  {
    idx = std::size_t(v & 0xFFFFFFFFULL);
    ctr = std::size_t(v >> 32);
  }

  // Push a freed slot back onto the lock-free free-list
  void push_free_index(std::size_t index)
  {
    std::uint64_t old_head = free_list_head.load(std::memory_order_relaxed);
    std::uint64_t new_head;
    std::size_t old_idx, old_ctr;

    do
    {
      unpack_index_counter(old_head, old_idx, old_ctr);
      data[index].next_free_index.store(old_idx, std::memory_order_relaxed);
      new_head = pack_index_counter(index, old_ctr + 1);
    } while (!free_list_head.compare_exchange_weak(
      old_head, new_head,
      std::memory_order_release,
      std::memory_order_relaxed));
  }

  // Pop a free slot; returns false if none remain
  bool pop_free_index(std::size_t& index)
  {
    std::uint64_t old_head = free_list_head.load(std::memory_order_relaxed);
    std::uint64_t new_head;
    std::size_t old_idx, old_ctr;

    do
    {
      unpack_index_counter(old_head, old_idx, old_ctr);

      if (old_idx == INVALID_INDEX)
      {
        return false;
      }

      index = old_idx;
      std::size_t next_idx = data[index].next_free_index.load(std::memory_order_relaxed);

      new_head = pack_index_counter(next_idx, old_ctr + 1);
    } while (!free_list_head.compare_exchange_weak(
      old_head, new_head,
      std::memory_order_acquire,
      std::memory_order_relaxed));

    return true;
  }

public:
  struct Op_Result
  {
    std::size_t index;
    T& value;
  };

  // Insert an element by perfect-forwarding constructor args.
  // Returns {index, reference} or nullopt if full/raced.
  template<typename... Args>
  std::optional<Op_Result> insert(Args&&... args)
  {
    std::size_t idx;
    if (!pop_free_index(idx))
    {
      return std::nullopt;
    }

    Entry& e = data[idx];

    // 1) CAS EMPTY -> INIT (capture current ABA counter)
    std::uint32_t old_st = e.state.load(std::memory_order_relaxed);
    std::uint32_t init_st;

    do
    {
      std::uint32_t ctr = old_st & ~Entry::STATE_MASK;

      if ((old_st & Entry::STATE_MASK) != Entry::EMPTY)
      {
        return std::nullopt; // Racing fail
      }

      init_st = ctr | Entry::INIT;
    } while (!e.state.compare_exchange_weak(
      old_st, init_st,
      std::memory_order_acq_rel,
      std::memory_order_relaxed));

    // 2) Construct T in-place
    T* ptr = reinterpret_cast<T*>(&e.storage);
    ::new (ptr) T(std::forward<Args>(args)...);

    // 3) Bump counter, mark READY
    std::uint32_t prev_ctr = init_st & ~Entry::STATE_MASK;
    std::uint32_t bumped = (prev_ctr + (1u << 2)) & ~Entry::STATE_MASK;
    e.state.store(bumped | Entry::READY, std::memory_order_release);

    return Op_Result{ idx, *ptr };
  }

  // Erase by index. Returns true if slot was READY.
  bool erase(std::size_t idx)
  {
    if (idx >= Capacity)
    {
      return false;
    }

    Entry& e = data[idx];

    // 1) CAS READY -> REMOVING
    std::uint32_t old_st = e.state.load(std::memory_order_acquire);
    std::uint32_t rem_st;

    do
    {
      std::uint32_t state = old_st & Entry::STATE_MASK;
      std::uint32_t ctr = old_st & ~Entry::STATE_MASK;

      if (state != Entry::READY)
      {
        return false; // Nothing to erase
      }

      rem_st = ctr | Entry::REMOVING;
    } while (!e.state.compare_exchange_weak(
      old_st, rem_st,
      std::memory_order_acq_rel,
      std::memory_order_relaxed));

    // 2) Destroy in-place
    T* ptr = reinterpret_cast<T*>(&e.storage);
    ptr->~T();

    // 3) Bump counter, mark EMPTY
    std::uint32_t prev_ctr = rem_st & ~Entry::STATE_MASK;
    std::uint32_t bumped = (prev_ctr + (1u << 2)) & ~Entry::STATE_MASK;
    e.state.store(bumped | Entry::EMPTY, std::memory_order_release);

    // 4) Return slot to free list
    push_free_index(idx);
    return true;
  }

  // Find with predicate
  template<typename Predicate>
  std::optional<Op_Result> find_if(Predicate pred) const
  {
    for (std::size_t i = 0; i < Capacity; ++i)
    {
      std::uint32_t st = data[i].state.load(std::memory_order_acquire);

      if ((st & Entry::STATE_MASK) == Entry::READY)
      {
        T* ptr = const_cast<T*>(reinterpret_cast<T const*>(&data[i].storage));

        if (pred(*ptr))
        {
          return Op_Result{ i, *ptr };
        }
      }
    }

    return std::nullopt;
  }

  // Find by value
  std::optional<Op_Result> find(const T& value) const
  {
    return find_if([&](const T& v)
    {
      return v == value;
    });
  }

  // Access by index
  std::optional<Op_Result> at(std::size_t idx) const
  {
    if (idx >= Capacity)
    {
      return std::nullopt;
    }

    std::uint32_t st = data[idx].state.load(std::memory_order_acquire);

    if ((st & Entry::STATE_MASK) != Entry::READY)
    {
      return std::nullopt;
    }

    T* ptr = const_cast<T*>(reinterpret_cast<T const*>(&data[idx].storage));
    return Op_Result{ idx, *ptr };
  }

  // Count live elements (O(Capacity))
  std::size_t size() const
  {
    std::size_t cnt = 0;

    for (std::size_t i = 0; i < Capacity; ++i)
    {
      std::uint32_t st = data[i].state.load(std::memory_order_acquire);

      if ((st & Entry::STATE_MASK) == Entry::READY)
      {
        ++cnt;
      }
    }

    return cnt;
  }

  constexpr std::size_t capacity() const
  {
    return Capacity;
  }

  // Call f(index, value) for every live element.
  template<typename Func>
  void for_each(Func f) const
  {
    for (std::size_t i = 0; i < Capacity; ++i)
    {
      if (auto opt = at(i))
      {
        f(opt->index, opt->value);
      }
    }
  }

  Safe_Array()
  {
    // Initialize free list: 0->1->2->…->INVALID_INDEX
    for (std::size_t i = 0; i < Capacity - 1; ++i)
    {
      data[i].next_free_index.store(i + 1, std::memory_order_relaxed);
    }

    data[Capacity - 1]
      .next_free_index.store(INVALID_INDEX, std::memory_order_relaxed);
    free_list_head.store(pack_index_counter(0, 0), std::memory_order_relaxed);
  }

  ~Safe_Array()
  {
    for (std::size_t i = 0; i < Capacity; ++i)
    {
      std::uint32_t st = data[i].state.load(std::memory_order_acquire);

      if ((st & Entry::STATE_MASK) == Entry::READY)
      {
        reinterpret_cast<T*>(&data[i].storage)->~T();
      }
    }
  }
};

#endif // LOCKFREE_THREADSAFE_ARRAY
