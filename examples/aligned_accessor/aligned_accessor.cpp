/*
//@HEADER
// ************************************************************************
//
//                        Kokkos v. 2.0
//              Copyright (2019) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Christian R. Trott (crtrott@sandia.gov)
//
// ************************************************************************
//@HEADER
*/

#include <experimental/mdspan>

#include <cassert>
#include <chrono>
#include <cstdlib> // aligned_alloc, posix_memalign (if applicable)
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <type_traits>

// mfh 2022/08/08: This is based on my comment on RAPIDS RAFT issue 725:
// https://github.com/rapidsai/raft/pull/725#discussion_r937991701

namespace {

using test_value_type = float;
constexpr int min_overalignment_factor = 8;
constexpr int min_byte_alignment =
  static_cast<int>(min_overalignment_factor * sizeof(float));

// Use int, not size_t, as the index_type.
// Some compilers have trouble optimizing loops with unsigned or 64-bit index types.
using index_type = int;

namespace stdex = std::experimental;

// Prefer std::assume_aligned if available, as it is in the C++ Standard.
// Otherwise, use a compiler-specific equivalent if available.

// NOTE (mfh 2022/08/08) BYTE_ALIGNMENT must be unsigned and a power of 2.
#if defined(__cpp_lib_assume_aligned)
#  define _MDSPAN_ASSUME_ALIGNED( ELEMENT_TYPE, POINTER, BYTE_ALIGNMENT ) (std::assume_aligned< BYTE_ALIGNMENT >( POINTER ))
  constexpr char assume_aligned_method[] = "std::assume_aligned";
#elif defined(__ICL)
#  define _MDSPAN_ASSUME_ALIGNED( ELEMENT_TYPE, POINTER, BYTE_ALIGNMENT ) POINTER
  constexpr char assume_aligned_method[] = "(none)";
#elif defined(__ICC)
#  define _MDSPAN_ASSUME_ALIGNED( ELEMENT_TYPE, POINTER, BYTE_ALIGNMENT ) POINTER
  constexpr char assume_aligned_method[] = "(none)";
#elif defined(__clang__)
#  define _MDSPAN_ASSUME_ALIGNED( ELEMENT_TYPE, POINTER, BYTE_ALIGNMENT ) POINTER
  constexpr char assume_aligned_method[] = "(none)";
#elif defined(__GNUC__)
  // __builtin_assume_aligned returns void*
#  define _MDSPAN_ASSUME_ALIGNED( ELEMENT_TYPE, POINTER, BYTE_ALIGNMENT ) reinterpret_cast< ELEMENT_TYPE* >(__builtin_assume_aligned( POINTER, BYTE_ALIGNMENT ))
  constexpr char assume_aligned_method[] = "__builtin_assume_aligned";
#else
#  define _MDSPAN_ASSUME_ALIGNED( ELEMENT_TYPE, POINTER, BYTE_ALIGNMENT ) POINTER
  constexpr char assume_aligned_method[] = "(none)";
#endif

// Some compilers other than Clang or GCC like to define __clang__ or __GNUC__.
// Thus, we order the tests from most to least specific.
#if defined(__ICL)
#  define _MDSPAN_ALIGN_VALUE_ATTRIBUTE( BYTE_ALIGNMENT ) __declspec(align_value( BYTE_ALIGNMENT ));
  constexpr char align_attribute_method[] = "__declspec(align_value(BYTE_ALIGNMENT))";
#elif defined(__ICC)
#  define _MDSPAN_ALIGN_VALUE_ATTRIBUTE( BYTE_ALIGNMENT ) __attribute__((align_value( BYTE_ALIGNMENT )));
  constexpr char align_attribute_method[] = "__attribute__((align_value(BYTE_ALIGNMENT)))";
#elif defined(__clang__)
#  define _MDSPAN_ALIGN_VALUE_ATTRIBUTE( BYTE_ALIGNMENT ) __attribute__((align_value( BYTE_ALIGNMENT )));
  constexpr char align_attribute_method[] = "__attribute__((align_value(BYTE_ALIGNMENT)))";
#else
#  define _MDSPAN_ALIGN_VALUE_ATTRIBUTE( BYTE_ALIGNMENT )
  constexpr char align_attribute_method[] = "(none)";
#endif

template<class ElementType>
constexpr bool
valid_byte_alignment(const std::size_t byte_alignment,
		     const std::size_t size_of_element)
{
  return byte_alignment >= alignof(ElementType); // && is a power of two
}

template<class ElementType, std::size_t byte_alignment>
struct aligned_pointer {
  // x86-64 ICC 2021.5.0 emits warning #3186 ("expected typedef declaration") here.
  // No other compiler (including Clang, which has a similar type attribute) has this issue.
  using type = ElementType* _MDSPAN_ALIGN_VALUE_ATTRIBUTE( byte_alignment );
};

template<class ElementType, std::size_t byte_alignment>
using aligned_pointer_t = typename aligned_pointer<ElementType, byte_alignment>::type;

template<class ElementType, std::size_t byte_alignment>
aligned_pointer_t<ElementType, byte_alignment>
bless(ElementType* ptr, std::integral_constant<std::size_t, byte_alignment> /* ba */ )
{
  return _MDSPAN_ASSUME_ALIGNED( ElementType, ptr, byte_alignment );
}

template<class ElementType, std::size_t byte_alignment>
struct aligned_accessor {
  using offset_policy = stdex::default_accessor<ElementType>;
  using element_type = ElementType;
  using reference = ElementType&;
  using data_handle_type = aligned_pointer_t<ElementType, byte_alignment>;

  constexpr aligned_accessor() noexcept = default;

  MDSPAN_TEMPLATE_REQUIRES(
    class OtherElementType,
    std::size_t other_byte_alignment,
    /* requires */ (std::is_convertible<OtherElementType(*)[], element_type(*)[]>::value && other_byte_alignment == byte_alignment)
    )
  constexpr aligned_accessor(aligned_accessor<OtherElementType, other_byte_alignment>) noexcept {}

  constexpr reference access(data_handle_type p, size_t i) const noexcept {
    // This may declare alignment twice, depending on
    // if we have an attribute for marking pointer types.
    return _MDSPAN_ASSUME_ALIGNED( ElementType, p, byte_alignment )[i];
  }

  constexpr typename offset_policy::data_handle_type
  offset(data_handle_type p, size_t i) const noexcept {
    return p + i;
  }
};

template<class ElementType>
struct delete_raw {
  void operator()(ElementType* p) const {
    if (p != nullptr) {
      // All the aligned allocation methods below go with std::free.
      // If we implement a new method that uses a different
      // deallocation function, that function would go here.
      std::free(p);
    }
  }
};

template<class ElementType>
using allocation_t = std::unique_ptr<
    ElementType[],
    delete_raw<ElementType>
  >;

template<class ElementType, std::size_t byte_alignment>
allocation_t<ElementType>
allocate_raw(const std::size_t num_elements)
{
  static_assert(byte_alignment >= sizeof(ElementType),
		"byte_alignment must be at least sizeof(ElementType).");
  static constexpr std::size_t overalignment = byte_alignment / sizeof(ElementType);
  static_assert(overalignment * sizeof(ElementType) == byte_alignment,
		"overalignment * sizeof(ElementType) must equal byte_alignment.");

  // Allocate an extra element, so we can do unaligned performance runs.
  const std::size_t num_bytes = (num_elements + 1) * sizeof(ElementType);
  auto deleter = delete_raw<ElementType>{};

  void* ptr = nullptr;
#ifdef _MSC_VER
  // MSVC 19.32 (or "latest" on godbolt.org) does NOT have aligned_alloc,
  // even with /std:c++latest.  Instead, we use MSVC-specific _aligned_malloc.
  ptr = _aligned_malloc(num_bytes, byte_alignment);
#elif __cplusplus < 201703L
  // aligned_alloc is a C11 function.  GCC does not provide it with -std=c++14.
  // We have coverage for the Windows case (at least with MSVC) above,
  // so we can resort to the POSIX function posix_memalign.
  const int err = posix_memalign(&ptr, byte_alignment, num_bytes);
  if(err != 0) {
    if(err == EINVAL) {
      throw std::runtime_error("posix_memalign failed: alignment not a power of two");
    } else if(err == ENOMEM) {
      throw std::runtime_error("posix_memalign failed: insufficient memory");
    } else {
      throw std::runtime_error("posix_memalign failed: unknown error");
    }
  }
#else
  ptr = std::aligned_alloc(byte_alignment, num_bytes);
#endif

  return {reinterpret_cast<ElementType*>(ptr), deleter};
}

template<class ElementType, std::size_t byte_alignment>
struct aligned_array_allocation {
  static_assert(byte_alignment >= sizeof(ElementType),
		"byte_alignment must be at least sizeof(ElementType).");
  static constexpr std::size_t overalignment = byte_alignment / sizeof(ElementType);
  static_assert(overalignment * sizeof(ElementType) == byte_alignment,
		"overalignment * sizeof(ElementType) must equal byte_alignment.");

  allocation_t<ElementType> allocation{nullptr, delete_raw<ElementType>{}};
  aligned_pointer_t<ElementType, byte_alignment> aligned_pointer = nullptr;
  std::size_t num_elements = 0;
};

template<class ElementType, std::size_t byte_alignment>
aligned_array_allocation<ElementType, byte_alignment>
allocate_aligned(std::size_t num_elements)
{
  if(num_elements == 0) {
    return {};
  }
  auto allocation = allocate_raw<ElementType, byte_alignment>(num_elements);
  aligned_pointer_t<ElementType, byte_alignment> aligned_pointer =
    _MDSPAN_ASSUME_ALIGNED( ElementType, allocation.get(), byte_alignment );
  return {std::move(allocation), aligned_pointer, num_elements};
}

// A dynamically allocated array of ElementType,
// whose zeroth element has byte alignment byte_alignment.
//
// ElementType: A trivially copyable type whose size is a power of two bytes.
//
// byte_alignment: A power of two that is a multiple of sizeof(ElementType).
template<class ElementType, std::size_t byte_alignment>
class aligned_array {
public:
  aligned_array(std::size_t num_elements) :
    alloc(allocate_aligned<ElementType, byte_alignment>(num_elements))
  {}

  // A pointer to the zeroth element of the array.
  // It is aligned to byte_alignment bytes.
  // All num_elements (constructor parameter) elements are valid.
  aligned_pointer_t<ElementType, byte_alignment>
  data_aligned() const noexcept
  {
    return _MDSPAN_ASSUME_ALIGNED( ElementType, alloc.aligned_pointer, byte_alignment );
  }

  // A pointer to the first (NOT zeroth) element of the array.
  // It is aligned only to sizeof(ElementType) bytes.
  // All num_elements (constructor parameter) elements are valid.
  //
  // This exists so that we can compare performance
  // of aligned and unaligned access.
  ElementType* data_unaligned() const noexcept
  {
    if (alloc.num_elements == 0) {
      return nullptr;
    } else { // align only to ElementType
      return &(alloc.allocation[1]);
    }
  }

private:
  aligned_array_allocation<ElementType, byte_alignment> alloc;
};

template<class ElementType, std::size_t byte_alignment>
using aligned_mdspan_1d =
  stdex::mdspan<ElementType,
		stdex::dextents<index_type, 1>,
		stdex::layout_right,
		aligned_accessor<ElementType, byte_alignment>>;

template<class ElementType>
using mdspan_1d =
  stdex::mdspan<ElementType,
		stdex::dextents<index_type, 1>,
		stdex::layout_right,
		stdex::default_accessor<ElementType>>;

#define TICK() const auto tick = std::chrono::steady_clock::now()

using double_seconds = std::chrono::duration<double, std::ratio<1>>;

#define TOCK() std::chrono::duration_cast<double_seconds>(std::chrono::steady_clock::now() - tick).count()

template<class ElementType, std::size_t byte_alignment>
void add_aligned_mdspan_1d(aligned_mdspan_1d<const ElementType, byte_alignment> x,
			   aligned_mdspan_1d<const ElementType, byte_alignment> y,
			   aligned_mdspan_1d<ElementType, byte_alignment> z)
{
  const index_type n = z.extent(0);
  for (index_type i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
  }
}

template<class ElementType, std::size_t byte_alignment>
auto benchmark_add_aligned_mdspan_1d(const std::size_t num_trials,
				     const index_type n,
				     const ElementType x[],
				     const ElementType y[],
				     ElementType z[],
				     std::integral_constant<std::size_t, byte_alignment> /* ba */ )
{
  TICK();
  aligned_mdspan_1d<const ElementType, byte_alignment> x2{x, n};
  aligned_mdspan_1d<const ElementType, byte_alignment> y2{y, n};
  aligned_mdspan_1d<ElementType, byte_alignment> z2{z, n};
  for (std::size_t trial = 0; trial < num_trials; ++trial) {
    add_aligned_mdspan_1d(x2, y2, z2);
  }
  return TOCK();
}

template<class ElementType>
void add_mdspan_1d(mdspan_1d<const ElementType> x,
		   mdspan_1d<const ElementType> y,
		   mdspan_1d<ElementType> z)
{
  const index_type n = z.extent(0);
  for (index_type i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
  }
}

template<class ElementType>
auto benchmark_add_mdspan_1d(const std::size_t num_trials,
			     const index_type n,
			     const ElementType x[],
			     const ElementType y[],
			     ElementType z[])
{
  TICK();
  mdspan_1d<const ElementType> x2{x, n};
  mdspan_1d<const ElementType> y2{y, n};
  mdspan_1d<ElementType> z2{z, n};
  for (std::size_t trial = 0; trial < num_trials; ++trial) {
    add_mdspan_1d(x2, y2, z2);
  }
  return TOCK();
}

template<class ElementType>
void add_raw_1d(const index_type n,
		const ElementType x[],
		const ElementType y[],
		ElementType z[])
{
  for (index_type i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
  }
}

template<class ElementType>
auto benchmark_add_raw_1d(const std::size_t num_trials,
			  const index_type n,
			  const ElementType x[],
			  const ElementType y[],
			  ElementType z[])
{
  TICK();
  for (std::size_t trial = 0; trial < num_trials; ++trial) {
    add_raw_1d(n, x, y, z);
  }
  return TOCK();
}

// Assume that x, y, and z all have the same alignment.
template<class ElementType, std::size_t byte_alignment>
void add_aligned_raw_1d(const index_type n,
			aligned_pointer_t<const ElementType, byte_alignment> x,
			aligned_pointer_t<const ElementType, byte_alignment> y,
			aligned_pointer_t<ElementType, byte_alignment> z,
			std::integral_constant<std::size_t, byte_alignment> /* ba */ )
{
  for (index_type i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
  }
}

// Assume that x, y, and z all have the same alignment.
template<class ElementType, std::size_t byte_alignment>
auto benchmark_add_aligned_raw_1d(const std::size_t num_trials,
				  const index_type n,
				  const ElementType x[],
				  const ElementType y[],
				  ElementType z[],
				  std::integral_constant<std::size_t, byte_alignment> ba)
{
  TICK();
  auto x_blessed = bless(x, ba);
  auto y_blessed = bless(y, ba);
  auto z_blessed = bless(z, ba);
  for (std::size_t trial = 0; trial < num_trials; ++trial) {
    add_aligned_raw_1d<ElementType, byte_alignment>(n, x_blessed, y_blessed, z_blessed, ba);
  }
  return TOCK();
}

#ifdef _OPENMP
template<class ElementType, std::size_t byte_alignment>
void add_omp_simd_aligned_mdspan_1d(aligned_mdspan_1d<const ElementType, byte_alignment> x,
				    aligned_mdspan_1d<const ElementType, byte_alignment> y,
				    aligned_mdspan_1d<ElementType, byte_alignment> z)
{
  const index_type n = z.extent(0);
  // Test whether OpenMP can figure out that the pointers are aligned.
#pragma omp simd
  for (index_type i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
  }
}

template<class ElementType, std::size_t byte_alignment>
auto benchmark_add_omp_simd_aligned_mdspan_1d(const std::size_t num_trials,
					      const index_type n,
					      const ElementType x[],
					      const ElementType y[],
					      ElementType z[],
					      std::integral_constant<std::size_t, byte_alignment> /* ba */ )
{
  TICK();
  aligned_mdspan_1d<const ElementType, byte_alignment> x2{x, n};
  aligned_mdspan_1d<const ElementType, byte_alignment> y2{y, n};
  aligned_mdspan_1d<ElementType, byte_alignment> z2{z, n};
  for (std::size_t trial = 0; trial < num_trials; ++trial) {
    add_omp_simd_aligned_mdspan_1d(x2, y2, z2);
  }
  return TOCK();
}

template<class ElementType>
void add_omp_simd_mdspan_1d(mdspan_1d<const ElementType> x,
			    mdspan_1d<const ElementType> y,
			    mdspan_1d<ElementType> z)
{
  const index_type n = z.extent(0);
#pragma omp simd
  for (index_type i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
  }
}

template<class ElementType>
auto benchmark_add_omp_simd_mdspan_1d(const std::size_t num_trials,
				      const index_type n,
				      const ElementType x[],
				      const ElementType y[],
				      ElementType z[])
{
  TICK();
  mdspan_1d<const ElementType> x2{x, n};
  mdspan_1d<const ElementType> y2{y, n};
  mdspan_1d<ElementType> z2{z, n};
  for (std::size_t trial = 0; trial < num_trials; ++trial) {
    add_omp_simd_mdspan_1d(x2, y2, z2);
  }
  return TOCK();
}

template<class ElementType>
void add_omp_simd_raw_1d(const index_type n,
			 const ElementType x[],
			 const ElementType y[],
			 ElementType z[])
{
#pragma omp simd
  for (index_type i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
  }
}

template<class ElementType>
auto benchmark_add_omp_simd_raw_1d(const std::size_t num_trials,
				   const index_type n,
				   const ElementType x[],
				   const ElementType y[],
				   ElementType z[])
{
  TICK();
  for (std::size_t trial = 0; trial < num_trials; ++trial) {
    add_omp_simd_raw_1d(n, x, y, z);
  }
  return TOCK();
}

template<class ElementType, std::size_t byte_alignment>
void add_omp_aligned_simd_raw_1d(const index_type n,
				 const ElementType x[],
				 const ElementType y[],
				 ElementType z[],
				 std::integral_constant<std::size_t, byte_alignment> /* ba */ )
{
  // The "aligned" clause might only work
  // for pointers or (raw) arrays.
  // C++23 adopting mdspan might change that,
  // at least for the default layout and accessor.
#pragma omp simd aligned(z,x,y:byte_alignment)
  for (index_type i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
  }
}

template<class ElementType, std::size_t byte_alignment>
auto benchmark_add_omp_aligned_simd_raw_1d(const std::size_t num_trials,
					   const index_type n,
					   const ElementType x[],
					   const ElementType y[],
					   ElementType z[],
					   std::integral_constant<std::size_t, byte_alignment> ba)
{
  TICK();
  for (std::size_t trial = 0; trial < num_trials; ++trial) {
    add_omp_aligned_simd_raw_1d(n, x, y, z, ba);
  }
  return TOCK();
}

template<class ElementType, std::size_t byte_alignment>
void add_omp_simd_aligned_raw_1d(const index_type n,
				 aligned_pointer_t<const ElementType, byte_alignment> x,
				 aligned_pointer_t<const ElementType, byte_alignment> y,
				 aligned_pointer_t<ElementType, byte_alignment> z)
{
#pragma omp simd
  for (index_type i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
  }
}

// Assume that x, y, and z all have the same alignment.
template<class ElementType, std::size_t byte_alignment>
auto benchmark_add_omp_simd_aligned_raw_1d(const std::size_t num_trials,
					   const index_type n,
					   const ElementType x[],
					   const ElementType y[],
					   ElementType z[],
					   std::integral_constant<std::size_t, byte_alignment> ba)
{
  TICK();
  auto x_blessed = bless(x, ba);
  auto y_blessed = bless(y, ba);
  auto z_blessed = bless(z, ba);
  for (std::size_t trial = 0; trial < num_trials; ++trial) {
    add_omp_simd_aligned_raw_1d<ElementType, byte_alignment>(n, x_blessed, y_blessed, z_blessed);
  }
  return TOCK();
}

template<class ElementType, std::size_t byte_alignment>
void add_omp_aligned_simd_aligned_raw_1d(const index_type n,
					 aligned_pointer_t<const ElementType, byte_alignment> x,
					 aligned_pointer_t<const ElementType, byte_alignment> y,
					 aligned_pointer_t<ElementType, byte_alignment> z)
{
#pragma omp simd aligned(z,x,y:byte_alignment)
  for (index_type i = 0; i < n; ++i) {
    z[i] = x[i] + y[i];
  }
}

// Assume that x, y, and z all have the same alignment.
template<class ElementType, std::size_t byte_alignment>
auto benchmark_add_omp_aligned_simd_aligned_raw_1d(
  const std::size_t num_trials,
  const index_type n,
  const ElementType x[],
  const ElementType y[],
  ElementType z[],
  std::integral_constant<std::size_t, byte_alignment> ba)
{
  TICK();
  auto x_blessed = bless(x, ba);
  auto y_blessed = bless(y, ba);
  auto z_blessed = bless(z, ba);
  for (std::size_t trial = 0; trial < num_trials; ++trial) {
    // Passing in ba doesn't help the compiler
    // deduce the template parameters,
    // so we just specify them explicitly.
    add_omp_aligned_simd_aligned_raw_1d<ElementType, byte_alignment>(n, x_blessed, y_blessed, z_blessed);
  }
  return TOCK();
}
#endif // _OPENMP

template<class ElementType>
auto warmup(const std::size_t num_trials,
	    const index_type n,
	    const ElementType x[],
	    const ElementType y[],
	    ElementType z[])
{
  return benchmark_add_raw_1d(num_trials, n, x, y, z);
}

} // namespace (anonymous)

int main(int argc, char* argv[])
{
  using std::cout;
  using std::cerr;
  using std::endl;

  if(argc != 3) {
    cerr << "Usage: main <n> <num_trials>" << endl;
    return -1;
  }

  int n = 10000;
  int num_trials = 100;
  n = std::stoi(argv[1]);
  num_trials = std::stoi(argv[2]);

  aligned_array<float, min_byte_alignment> x(n);
  aligned_array<float, min_byte_alignment> y(n);
  aligned_array<float, min_byte_alignment> z(n);

  constexpr std::integral_constant<std::size_t, min_byte_alignment> byte_alignment;

  auto warmup_result =
    warmup(num_trials, n, x.data_unaligned(), y.data_unaligned(), z.data_unaligned());

  auto aligned_mdspan_result =
    benchmark_add_aligned_mdspan_1d(num_trials, n, x.data_aligned(),
				    y.data_aligned(), z.data_aligned(),
				    byte_alignment);
  auto mdspan_result =
    benchmark_add_mdspan_1d(num_trials, n, x.data_unaligned(),
			    y.data_unaligned(), z.data_unaligned());
  auto raw_result =
    benchmark_add_raw_1d(num_trials, n, x.data_unaligned(),
			 y.data_unaligned(), z.data_unaligned());
  auto aligned_raw_result =
    benchmark_add_aligned_raw_1d(num_trials, n, x.data_aligned(),
				 y.data_aligned(), z.data_aligned(),
				 byte_alignment);

#ifdef _OPENMP
  auto omp_simd_aligned_mdspan_result =
    benchmark_add_omp_simd_aligned_mdspan_1d(num_trials, n, x.data_aligned(),
					     y.data_aligned(), z.data_aligned(),
					     byte_alignment);
  auto omp_simd_mdspan_result =
    benchmark_add_omp_simd_mdspan_1d(num_trials, n, x.data_unaligned(),
				     y.data_unaligned(), z.data_unaligned());
  auto omp_simd_raw_result =
    benchmark_add_omp_simd_raw_1d(num_trials, n, x.data_unaligned(),
				  y.data_unaligned(), z.data_unaligned());
  auto omp_aligned_simd_raw_result =
    benchmark_add_omp_aligned_simd_raw_1d(num_trials, n,
					  x.data_aligned(), y.data_aligned(), z.data_aligned(),
					  byte_alignment);
  auto omp_simd_aligned_raw_result =
    benchmark_add_omp_simd_aligned_raw_1d(num_trials, n,
					  x.data_aligned(), y.data_aligned(), z.data_aligned(),
					  byte_alignment);
  auto omp_aligned_simd_aligned_raw_result =
    benchmark_add_omp_aligned_simd_aligned_raw_1d(num_trials, n,
						  x.data_aligned(), y.data_aligned(), z.data_aligned(),
						  byte_alignment);

#endif // _OPENMP

  cout << "Number of trials: " << num_trials << endl
       << "Number of loop iterations per trial: " << n << endl
       << "Way to declare a pointer value aligned, if any: "
       << assume_aligned_method << endl
       << "Way to declare a pointer type aligned, if any: "
       << align_attribute_method << endl
       << "Total time in seconds for non-OpenMP loops:" << endl
       << "  warmup: " << warmup_result << endl
       << "  mdspan with aligned_accessor: " << aligned_mdspan_result << endl
       << "  mdspan with default_accessor: " << mdspan_result << endl
       << "  raw arrays: " << raw_result << endl
       << "  aligned raw arrays: " << aligned_raw_result << endl;

#ifdef _OPENMP
  cout << "Total time in seconds for OpenMP (omp simd) loops:" << endl
       << "  omp_simd_aligned_mdspan: " << omp_simd_aligned_mdspan_result << endl
       << "  omp_simd_mdspan: " << omp_simd_mdspan_result << endl
       << "  omp_simd_raw: " << omp_simd_raw_result << endl
       << "  omp_aligned_simd_raw (aligned clause): "
       << omp_aligned_simd_raw_result << endl
       << "  omp_simd_aligned_raw (pointer declarations): "
       << omp_simd_aligned_raw_result << endl
       << "  omp_aligned_simd_aligned_raw (both): "
       << omp_aligned_simd_aligned_raw_result << endl;
#endif // _OPENMP
  return 0;
}
