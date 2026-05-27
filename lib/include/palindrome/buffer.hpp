#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

// A fixed-capacity, over-aligned sample buffer for the streaming DSP path.
//
// Deliberately *not* a std::vector and deliberately not growable element by
// element: there is no push_back. In the DSP pipeline every stage knows its
// exact output count up front (process() computes it; max_output_for() bounds it
// for sizing), so the hot path
// is "give me a writable span of exactly N" -- a counted store loop with no
// per-element capacity check, which is what lets the compiler keep the output
// pointer loop-invariant and vectorise the store. The one allocation happens at
// setup time via reserve(); write_n() and clear() never touch the allocator.
//
// The storage is aligned to kAlign (64 bytes -> AVX-512-safe, harmless on AVX2),
// and data()/view() hand back std::assume_aligned pointers so loads and stores
// can be aligned.
namespace palindrome {

template<class T>
class Buffer {
public:
  static constexpr std::size_t kAlign = 64;
  static_assert(alignof(T) <= kAlign, "Buffer alignment must cover T's alignment");
  // The buffer hands out raw storage and never constructs or destroys elements:
  // write_n() returns a span the caller stores into (beginning the trivial
  // objects' lifetimes), reserve() relocates with a trivial copy, and the
  // destructor frees without running any. That contract is only sound for types
  // that need none of that machinery.
  static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
      "Buffer<T> requires a trivially copyable, trivially destructible T");

  Buffer() = default;
  explicit Buffer(std::size_t capacity) { reserve(capacity); }

  ~Buffer() { release(); }

  Buffer(Buffer &&o) noexcept :
      data_{std::exchange(o.data_, nullptr)}, size_{std::exchange(o.size_, 0)},
      capacity_{std::exchange(o.capacity_, 0)} {}

  Buffer &operator=(Buffer &&o) noexcept {
    if (this != &o) {
      release();
      data_ = std::exchange(o.data_, nullptr);
      size_ = std::exchange(o.size_, 0);
      capacity_ = std::exchange(o.capacity_, 0);
    }
    return *this;
  }

  Buffer(const Buffer &) = delete;
  Buffer &operator=(const Buffer &) = delete;

  // Allocate (once) room for at least `capacity` elements. Growing reallocates
  // and preserves the current contents; shrinking is a no-op. Intended for the
  // pipeline's setup phase -- not the streaming path.
  void reserve(std::size_t capacity) {
    if (capacity <= capacity_)
      return;
    T *fresh = allocate(capacity);
    std::uninitialized_copy_n(data_, size_, fresh);
    release();
    data_ = fresh;
    capacity_ = capacity;
  }

  // Hot path: set the logical size to exactly `n` and return an aligned span to
  // write into (its contents are unspecified). Throws std::length_error if `n`
  // exceeds capacity() -- the stage was handed a bigger block than prepare()
  // budgeted for. No allocation, no zero-init, no capacity branch in the loop.
  [[nodiscard]] std::span<T> write_n(std::size_t n) {
    if (n > capacity_)
      throw std::length_error("Buffer::write_n exceeds reserved capacity");
    size_ = n;
    return {data(), n};
  }

  void clear() noexcept { size_ = 0; }

  // assume_aligned's precondition is a genuinely aligned pointer; a null buffer
  // (never reserved) has none, so guard it. The branch is setup-time only -- the
  // hot path writes through the span write_n() returns.
  [[nodiscard]] T *data() noexcept { return data_ ? std::assume_aligned<kAlign>(data_) : nullptr; }
  [[nodiscard]] const T *data() const noexcept { return data_ ? std::assume_aligned<kAlign>(data_) : nullptr; }

  [[nodiscard]] std::span<T> span() noexcept { return {data(), size_}; }
  [[nodiscard]] std::span<const T> view() const noexcept { return {data(), size_}; }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

private:
  static T *allocate(std::size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
      throw std::length_error("Buffer allocation size overflows");
    return static_cast<T *>(::operator new(n * sizeof(T), std::align_val_t{kAlign}));
  }
  void release() noexcept {
    ::operator delete(data_, std::align_val_t{kAlign});
    data_ = nullptr;
  }

  T *data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
};

} // namespace palindrome
