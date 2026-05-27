#pragma once

#include <cstddef>

// A pointer wrapper that carries the no-aliasing contract in the *type*.
//
// `__restrict` is a qualifier, not part of a function's type, so a signature
// taking `float* __restrict` looks identical to one taking `float*` -- the
// promise lives in a comment, not the interface. restrict_ptr<T> instead holds
// the qualifier on its member, so a parameter of type restrict_ptr<float> states
// "I assume this doesn't alias my other pointers" in the signature itself.
//
// The qualifier living on the member (not a per-dereference local) is what makes
// it work: __restrict's guarantee is scoped to the qualified pointer's lifetime,
// so it must span the whole loop to license vectorisation. Measured on GCC 15,
// a kernel taking restrict_ptr params generates byte-identical code to one
// taking raw `float* __restrict` -- including through a per-function
// [[gnu::optimize]] attribute, where operator[] still inlines (so no
// always_inline is needed). __restrict is a GCC/Clang extension, not standard.
namespace palindrome {

template<class T>
class restrict_ptr {
public:
  constexpr restrict_ptr() noexcept = default;
  constexpr restrict_ptr(T *p) noexcept : p_{p} {} // implicit: a raw pointer converts at the call site

  [[nodiscard]] constexpr T &operator*() const noexcept { return *p_; }
  [[nodiscard]] constexpr T *operator->() const noexcept { return p_; }
  [[nodiscard]] constexpr T &operator[](std::size_t k) const noexcept { return p_[k]; }

  constexpr restrict_ptr &operator++() noexcept {
    ++p_;
    return *this;
  }
  constexpr restrict_ptr operator++(int) noexcept {
    restrict_ptr t = *this;
    ++p_;
    return t;
  }
  constexpr restrict_ptr &operator--() noexcept {
    --p_;
    return *this;
  }
  constexpr restrict_ptr operator--(int) noexcept {
    restrict_ptr t = *this;
    --p_;
    return t;
  }
  constexpr restrict_ptr &operator+=(std::ptrdiff_t d) noexcept {
    p_ += d;
    return *this;
  }
  constexpr restrict_ptr &operator-=(std::ptrdiff_t d) noexcept {
    p_ -= d;
    return *this;
  }

  [[nodiscard]] friend constexpr restrict_ptr operator+(restrict_ptr p, std::ptrdiff_t d) noexcept { return p += d; }
  [[nodiscard]] friend constexpr restrict_ptr operator+(std::ptrdiff_t d, restrict_ptr p) noexcept { return p += d; }
  [[nodiscard]] friend constexpr restrict_ptr operator-(restrict_ptr p, std::ptrdiff_t d) noexcept { return p -= d; }
  [[nodiscard]] friend constexpr std::ptrdiff_t operator-(restrict_ptr a, restrict_ptr b) noexcept {
    return a.p_ - b.p_;
  }

  [[nodiscard]] constexpr T *get() const noexcept { return p_; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return p_ != nullptr; }

  [[nodiscard]] friend constexpr bool operator==(restrict_ptr a, restrict_ptr b) noexcept { return a.p_ == b.p_; }

private:
  T *__restrict p_ = nullptr;
};

} // namespace palindrome
