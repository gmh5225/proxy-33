// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef _MSFT_PROXY_
#define _MSFT_PROXY_

#include <bit>
#include <concepts>
#include <initializer_list>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace pro {

enum class constraint_level { none, nontrivial, nothrow, trivial };

struct proxiable_ptr_constraints {
  std::size_t max_size;
  std::size_t max_align;
  constraint_level copyability;
  constraint_level relocatability;
  constraint_level destructibility;
};
constexpr proxiable_ptr_constraints relocatable_ptr_constraints{
  .max_size = sizeof(void*) * 2u,
  .max_align = alignof(void*),
  .copyability = constraint_level::none,
  .relocatability = constraint_level::nothrow,
  .destructibility = constraint_level::nothrow,
};
constexpr proxiable_ptr_constraints copyable_ptr_constraints{
  .max_size = sizeof(void*) * 2u,
  .max_align = alignof(void*),
  .copyability = constraint_level::nontrivial,
  .relocatability = constraint_level::nothrow,
  .destructibility = constraint_level::nothrow,
};
constexpr proxiable_ptr_constraints trivial_ptr_constraints{
  .max_size = sizeof(void*),
  .max_align = alignof(void*),
  .copyability = constraint_level::trivial,
  .relocatability = constraint_level::trivial,
  .destructibility = constraint_level::trivial,
};

namespace details {

struct applicable_traits { static constexpr bool applicable = true; };
struct inapplicable_traits { static constexpr bool applicable = false; };

template <template <class, class> class R, class O, class... Is>
struct recursive_reduction : std::type_identity<O> {};
template <template <class, class> class R, class O, class I, class... Is>
struct recursive_reduction<R, O, I, Is...>
    : recursive_reduction<R, typename R<O, I>::type, Is...> {};
template <template <class, class> class R, class O, class... Is>
using recursive_reduction_t = typename recursive_reduction<R, O, Is...>::type;

template <class T>
consteval bool has_copyability(constraint_level level) {
  switch (level) {
    case constraint_level::trivial:
      return std::is_trivially_copy_constructible_v<T>;
    case constraint_level::nothrow:
      return std::is_nothrow_copy_constructible_v<T>;
    case constraint_level::nontrivial: return std::is_copy_constructible_v<T>;
    case constraint_level::none: return true;
    default: return false;
  }
}
template <class T>
consteval bool has_relocatability(constraint_level level) {
  switch (level) {
    case constraint_level::trivial:
      return std::is_trivially_move_constructible_v<T> &&
          std::is_trivially_destructible_v<T>;
    case constraint_level::nothrow:
      return std::is_nothrow_move_constructible_v<T> &&
          std::is_nothrow_destructible_v<T>;
    case constraint_level::nontrivial:
      return std::is_move_constructible_v<T> && std::is_destructible_v<T>;
    case constraint_level::none: return true;
    default: return false;
  }
}
template <class T>
consteval bool has_destructibility(constraint_level level) {
  switch (level) {
    case constraint_level::trivial: return std::is_trivially_destructible_v<T>;
    case constraint_level::nothrow: return std::is_nothrow_destructible_v<T>;
    case constraint_level::nontrivial: return std::is_destructible_v<T>;
    case constraint_level::none: return true;
    default: return false;
  }
}
consteval bool requires_lifetime_meta(constraint_level level) {
  return level > constraint_level::none && level < constraint_level::trivial;
}

// As per std::to_address() wording in [pointer.conversion]
template <class P> struct ptr_traits : inapplicable_traits {};
template <class P>
    requires(requires(const P p) { std::pointer_traits<P>::to_address(p); } ||
        requires(const P p) { p.operator->(); })
struct ptr_traits<P> : applicable_traits {
  static auto to_address(const P& p) noexcept { return std::to_address(p); }
  using reference_type = typename ptr_traits<
      decltype(to_address(std::declval<const P&>()))>::reference_type;
};
template <class T>
struct ptr_traits<T*> : applicable_traits {
  static auto to_address(T* p) noexcept { return p; }
  using reference_type = T&;
};

template <class O> struct overload_traits : inapplicable_traits {};
template <class R, class... Args>
struct overload_traits<R(Args...)> : applicable_traits {
  using dispatcher_type = R (*)(const char*, Args...);
  struct resolver { R (*operator()(Args...))(Args...); };
  using forwarding_argument_types = std::tuple<Args&&...>;  // For helper macros

  template <class D, class P>
  static constexpr bool applicable_ptr = std::is_invocable_v<
      D, typename ptr_traits<P>::reference_type, Args...>;
  static constexpr bool is_noexcept = false;
  template <class D, class P>
  static R dispatcher(const char* erased, Args... args) {
    auto ptr = ptr_traits<P>::to_address(*reinterpret_cast<const P*>(erased));
    if constexpr (std::is_void_v<R>) {
      D{}(*ptr, std::forward<Args>(args)...);
    } else {
      return D{}(*ptr, std::forward<Args>(args)...);
    }
  }
};
template <class R, class... Args>
struct overload_traits<R(Args...) noexcept> : applicable_traits {
  using dispatcher_type = R (*)(const char*, Args...) noexcept;
  struct resolver { R (*operator()(Args...))(Args...) noexcept; };
  using forwarding_argument_types = std::tuple<Args&&...>;  // For helper macros

  template <class D, class P>
  static constexpr bool applicable_ptr = std::is_nothrow_invocable_v<
      D, typename ptr_traits<P>::reference_type, Args...>;
  static constexpr bool is_noexcept = true;
  template <class D, class P>
  static R dispatcher(const char* erased, Args... args) noexcept {
    auto ptr = ptr_traits<P>::to_address(*reinterpret_cast<const P*>(erased));
    if constexpr (std::is_void_v<R>) {
      D{}(*ptr, std::forward<Args>(args)...);
    } else {
      return D{}(*ptr, std::forward<Args>(args)...);
    }
  }
};

template <class D, class Os>
struct dispatch_traits_impl : inapplicable_traits {};
template <class D, class... Os>
    requires(sizeof...(Os) > 0u && (overload_traits<Os>::applicable && ...))
struct dispatch_traits_impl<D, std::tuple<Os...>> : applicable_traits {
 private:
  struct overload_resolver : overload_traits<Os>::resolver...
      { using overload_traits<Os>::resolver::operator()...; };

 public:
  struct meta {
    template <class P>
    constexpr explicit meta(std::in_place_type_t<P>)
        : dispatchers(overload_traits<Os>::template dispatcher<D, P>...) {}

    std::tuple<typename overload_traits<Os>::dispatcher_type...> dispatchers;
  };
  template <class... Args>
  using matched_overload =
      std::remove_pointer_t<std::invoke_result_t<overload_resolver, Args...>>;

  template <class P>
  static constexpr bool applicable_ptr =
      (overload_traits<Os>::template applicable_ptr<D, P> && ...);
};
template <class D> struct dispatch_traits : inapplicable_traits {};
template <class D>
    requires(requires { typename D::overload_types; } &&
        std::is_trivially_default_constructible_v<D>)
struct dispatch_traits<D>
    : dispatch_traits_impl<D, typename D::overload_types> {};

template <class... Ms>
struct composite_meta : Ms... {
  template <class P>
  constexpr explicit composite_meta(std::in_place_type_t<P>)
      : Ms(std::in_place_type<P>)... {}
};
template <constraint_level C> struct copyability_meta_provider;
template <>
struct copyability_meta_provider<constraint_level::nontrivial> {
  template <class P>
  static void dispatcher(char* self, const char* rhs)
      { new(self) P(*reinterpret_cast<const P*>(rhs)); }
};
template <>
struct copyability_meta_provider<constraint_level::nothrow> {
  template <class P>
  static void dispatcher(char* self, const char* rhs) noexcept
      { new(self) P(*reinterpret_cast<const P*>(rhs)); }
};
template <constraint_level C> struct relocatability_meta_provider;
template <>
struct relocatability_meta_provider<constraint_level::nontrivial> {
  template <class P>
  static void dispatcher(char* self, char* rhs) {
    new(self) P(std::move(*reinterpret_cast<P*>(rhs)));
    reinterpret_cast<P*>(rhs)->~P();
  }
};
template <>
struct relocatability_meta_provider<constraint_level::nothrow> {
  template <class P>
  static void dispatcher(char* self, char* rhs) noexcept {
    new(self) P(std::move(*reinterpret_cast<P*>(rhs)));
    reinterpret_cast<P*>(rhs)->~P();
  }
};
template <constraint_level C> struct destructibility_meta_provider;
template <>
struct destructibility_meta_provider<constraint_level::nontrivial> {
  template <class P>
  static void dispatcher(char* self) { reinterpret_cast<P*>(self)->~P(); }
};
template <>
struct destructibility_meta_provider<constraint_level::nothrow> {
  template <class P>
  static void dispatcher(char* self) noexcept
      { reinterpret_cast<P*>(self)->~P(); }
};
template <template <constraint_level> class MP, constraint_level C>
struct lifetime_meta_impl {
  template <class P>
  constexpr explicit lifetime_meta_impl(std::in_place_type_t<P>)
      : dispatcher(&MP<C>::template dispatcher<P>) {}

  decltype(&MP<C>::template dispatcher<void>) dispatcher;
};
template <template <constraint_level> class MP, constraint_level C>
using lifetime_meta = std::conditional_t<
    requires_lifetime_meta(C), lifetime_meta_impl<MP, C>, void>;

template <class O, class I>
struct facade_meta_reduction : std::type_identity<O> {};
template <class... Ms, class I> requires(!std::is_void_v<I>)
struct facade_meta_reduction<composite_meta<Ms...>, I>
    : std::type_identity<composite_meta<Ms..., I>> {};

template <class... Ds>
struct default_dispatch_traits { using default_dispatch = void; };
template <class D>
struct default_dispatch_traits<D> { using default_dispatch = D; };
template <class F, class Ds>
struct basic_facade_traits_impl : inapplicable_traits {};
template <class F, class... Ds>
struct basic_facade_traits_impl<F, std::tuple<Ds...>>
    : applicable_traits, default_dispatch_traits<Ds...> {
  using copyability_meta = lifetime_meta<
      copyability_meta_provider, F::constraints.copyability>;
  using relocatability_meta = lifetime_meta<
      relocatability_meta_provider, F::constraints.relocatability>;
  using destructibility_meta = lifetime_meta<
      destructibility_meta_provider, F::constraints.destructibility>;
  using meta = recursive_reduction_t<facade_meta_reduction,
      composite_meta<>, copyability_meta, relocatability_meta,
      destructibility_meta, typename F::reflection_type>;

  template <class D>
  static constexpr bool has_dispatch = (std::is_same_v<D, Ds> || ...);
};
template <class F> struct basic_facade_traits : inapplicable_traits {};
template <class F>
    requires(
        requires {
          typename F::dispatch_types;
          F::constraints;
          typename F::reflection_type;
        } &&
        std::is_same_v<decltype(F::constraints),
            const proxiable_ptr_constraints> &&
        std::has_single_bit(F::constraints.max_align) &&
        F::constraints.max_size % F::constraints.max_align == 0u &&
        (std::is_void_v<typename F::reflection_type> ||
            std::is_trivially_copyable_v<typename F::reflection_type>))
struct basic_facade_traits<F>
    : basic_facade_traits_impl<F, typename F::dispatch_types> {};

template <class F, class Ds>
struct facade_traits_impl : inapplicable_traits {};
template <class F, class... Ds> requires(dispatch_traits<Ds>::applicable && ...)
struct facade_traits_impl<F, std::tuple<Ds...>> : applicable_traits {
  using meta = composite_meta<typename basic_facade_traits<F>::meta,
      typename dispatch_traits<Ds>::meta...>;

  template <class P>
  static constexpr bool applicable_ptr =
      sizeof(P) <= F::constraints.max_size &&
      alignof(P) <= F::constraints.max_align &&
      has_copyability<P>(F::constraints.copyability) &&
      has_relocatability<P>(F::constraints.relocatability) &&
      has_destructibility<P>(F::constraints.destructibility) &&
      (dispatch_traits<Ds>::template applicable_ptr<P> && ...) &&
      (std::is_void_v<typename F::reflection_type> || std::is_constructible_v<
          typename F::reflection_type, std::in_place_type_t<P>>);
  template <class P> static constexpr meta meta_storage{std::in_place_type<P>};
};
template <class F>
struct facade_traits : facade_traits_impl<F, typename F::dispatch_types> {};

}  // namespace details

template <class F>
concept basic_facade = details::basic_facade_traits<F>::applicable;

template <class F>
concept facade = basic_facade<F> && details::facade_traits<F>::applicable;

template <class P, class F>
concept proxiable = facade<F> && details::ptr_traits<P>::applicable &&
    details::facade_traits<F>::template applicable_ptr<P>;

template <basic_facade F>
class proxy {
  using BasicTraits = details::basic_facade_traits<F>;
  using Traits = details::facade_traits<F>;
  using DefaultDispatch = typename BasicTraits::default_dispatch;
  template <class D, class... Args>
  using MatchedOverload =
      typename details::dispatch_traits<D>::template matched_overload<Args...>;

  template <class P, class... Args>
  static constexpr bool HasNothrowPolyConstructor = std::conditional_t<
      proxiable<P, F>, std::is_nothrow_constructible<P, Args...>,
          std::false_type>::value;
  template <class P, class... Args>
  static constexpr bool HasPolyConstructor = std::conditional_t<
      proxiable<P, F>, std::is_constructible<P, Args...>,
          std::false_type>::value;
  static constexpr bool HasTrivialCopyConstructor =
      F::constraints.copyability == constraint_level::trivial;
  static constexpr bool HasNothrowCopyConstructor =
      F::constraints.copyability >= constraint_level::nothrow;
  static constexpr bool HasCopyConstructor =
      F::constraints.copyability >= constraint_level::nontrivial;
  static constexpr bool HasNothrowMoveConstructor =
      F::constraints.relocatability >= constraint_level::nothrow;
  static constexpr bool HasMoveConstructor =
      F::constraints.relocatability >= constraint_level::nontrivial;
  static constexpr bool HasTrivialDestructor =
      F::constraints.destructibility == constraint_level::trivial;
  static constexpr bool HasNothrowDestructor =
      F::constraints.destructibility >= constraint_level::nothrow;
  static constexpr bool HasDestructor =
      F::constraints.destructibility >= constraint_level::nontrivial;
  template <class P, class... Args>
  static constexpr bool HasNothrowPolyAssignment =
      HasNothrowPolyConstructor<P, Args...> && HasNothrowDestructor;
  template <class P, class... Args>
  static constexpr bool HasPolyAssignment = HasPolyConstructor<P, Args...> &&
      HasDestructor;
  static constexpr bool HasTrivialCopyAssignment = HasTrivialCopyConstructor &&
      HasTrivialDestructor;
  static constexpr bool HasNothrowCopyAssignment = HasNothrowCopyConstructor &&
      HasNothrowDestructor;
  static constexpr bool HasCopyAssignment = HasNothrowCopyAssignment ||
      (HasCopyConstructor && HasMoveConstructor && HasDestructor);
  static constexpr bool HasNothrowMoveAssignment = HasNothrowMoveConstructor &&
      HasNothrowDestructor;
  static constexpr bool HasMoveAssignment = HasMoveConstructor && HasDestructor;
  template <class D, class... Args>
  static constexpr bool HasNothrowInvocation =
      details::overload_traits<MatchedOverload<D, Args...>>::is_noexcept;

 public:
  proxy() noexcept { meta_ = nullptr; }
  proxy(std::nullptr_t) noexcept : proxy() {}
  proxy(const proxy& rhs) noexcept(HasNothrowCopyConstructor)
      requires(!HasTrivialCopyConstructor && HasCopyConstructor) {
    if (rhs.meta_ != nullptr) {
      rhs.meta_->BasicTraits::copyability_meta::dispatcher(ptr_, rhs.ptr_);
      meta_ = rhs.meta_;
    } else {
      meta_ = nullptr;
    }
  }
  proxy(const proxy&) noexcept requires(HasTrivialCopyConstructor) = default;
  proxy(const proxy&) requires(!HasCopyConstructor) = delete;
  proxy(proxy&& rhs) noexcept(HasNothrowMoveConstructor)
      requires(HasMoveConstructor) {
    if (rhs.meta_ != nullptr) {
      if constexpr (F::constraints.relocatability ==
          constraint_level::trivial) {
        memcpy(ptr_, rhs.ptr_, F::constraints.max_size);
      } else {
        rhs.meta_->BasicTraits::relocatability_meta::dispatcher(ptr_, rhs.ptr_);
      }
      meta_ = rhs.meta_;
      rhs.meta_ = nullptr;
    } else {
      meta_ = nullptr;
    }
  }
  proxy(proxy&&) requires(!HasMoveConstructor) = delete;
  template <class P>
  proxy(P&& ptr) noexcept(HasNothrowPolyConstructor<std::decay_t<P>, P>)
      requires(HasPolyConstructor<std::decay_t<P>, P>)
      { initialize<std::decay_t<P>>(std::forward<P>(ptr)); }
  template <class P, class... Args>
  explicit proxy(std::in_place_type_t<P>, Args&&... args)
      noexcept(HasNothrowPolyConstructor<P, Args...>)
      requires(HasPolyConstructor<P, Args...>)
      { initialize<P>(std::forward<Args>(args)...); }
  template <class P, class U, class... Args>
  explicit proxy(std::in_place_type_t<P>, std::initializer_list<U> il,
          Args&&... args)
      noexcept(HasNothrowPolyConstructor<P, std::initializer_list<U>&, Args...>)
      requires(HasPolyConstructor<P, std::initializer_list<U>&, Args...>)
      { initialize<P>(il, std::forward<Args>(args)...); }
  proxy& operator=(std::nullptr_t) noexcept(HasNothrowDestructor)
      requires(HasDestructor) {
    this->~proxy();
    new(this) proxy();
    return *this;
  }
  proxy& operator=(const proxy& rhs)
      requires(!HasNothrowCopyAssignment && HasCopyAssignment)
      { return *this = proxy{rhs}; }
  proxy& operator=(const proxy& rhs) noexcept
      requires(!HasTrivialCopyAssignment && HasNothrowCopyAssignment) {
    if (this != &rhs) {
      this->~proxy();
      new(this) proxy(rhs);
    }
    return *this;
  }
  proxy& operator=(const proxy&) noexcept requires(HasTrivialCopyAssignment) =
      default;
  proxy& operator=(const proxy&) requires(!HasCopyAssignment) = delete;
  proxy& operator=(proxy&& rhs) noexcept(HasNothrowMoveAssignment)
    requires(HasMoveAssignment) {
    if (this != &rhs) {
      if constexpr (HasNothrowMoveAssignment) {
        this->~proxy();
      } else {
        reset();  // For weak exception safety
      }
      new(this) proxy(std::move(rhs));
    }
    return *this;
  }
  proxy& operator=(proxy&&) requires(!HasMoveAssignment) = delete;
  template <class P>
  proxy& operator=(P&& ptr) noexcept
      requires(HasNothrowPolyAssignment<std::decay_t<P>, P>) {
    this->~proxy();
    initialize<std::decay_t<P>>(std::forward<P>(ptr));
    return *this;
  }
  template <class P>
  proxy& operator=(P&& ptr)
      requires(!HasNothrowPolyAssignment<std::decay_t<P>, P> &&
          HasPolyAssignment<std::decay_t<P>, P>)
      { return *this = proxy{std::forward<P>(ptr)}; }
  ~proxy() noexcept(HasNothrowDestructor)
      requires(!HasTrivialDestructor && HasDestructor) {
    if (meta_ != nullptr) {
      meta_->BasicTraits::destructibility_meta::dispatcher(ptr_);
    }
  }
  ~proxy() requires(HasTrivialDestructor) = default;
  ~proxy() requires(!HasDestructor) = delete;

  bool has_value() const noexcept { return meta_ != nullptr; }
  decltype(auto) reflect() const noexcept
      requires(!std::is_void_v<typename F::reflection_type>)
      { return static_cast<const typename F::reflection_type&>(*meta_); }
  void reset() noexcept(HasNothrowDestructor) requires(HasDestructor)
      { this->~proxy(); meta_ = nullptr; }
  void swap(proxy& rhs) noexcept(HasNothrowMoveConstructor)
      requires(HasMoveConstructor) {
    if constexpr (F::constraints.relocatability == constraint_level::trivial) {
      std::swap(meta_, rhs.meta_);
      std::swap(ptr_, rhs.ptr);
    } else {
      if (meta_ != nullptr) {
        if (rhs.meta_ != nullptr) {
          proxy temp = std::move(*this);
          new(this) proxy(std::move(rhs));
          new(&rhs) proxy(std::move(temp));
        } else {
          new(&rhs) proxy(std::move(*this));
        }
      } else if (rhs.meta_ != nullptr) {
        new(this) proxy(std::move(rhs));
      }
    }
  }
  friend void swap(proxy& a, proxy& b) noexcept(HasNothrowMoveConstructor)
      { a.swap(b); }
  template <class P, class... Args>
  P& emplace(Args&&... args) noexcept(HasNothrowPolyAssignment<P, Args...>)
      requires(HasPolyAssignment<P, Args...>) {
    reset();
    initialize<P>(std::forward<Args>(args)...);
    return *reinterpret_cast<P*>(ptr_);
  }
  template <class P, class U, class... Args>
  P& emplace(std::initializer_list<U> il, Args&&... args)
      noexcept(HasNothrowPolyAssignment<P, std::initializer_list<U>&, Args...>)
      requires(HasPolyAssignment<P, std::initializer_list<U>&, Args...>) {
    reset();
    initialize<P>(il, std::forward<Args>(args)...);
    return *reinterpret_cast<P*>(ptr_);
  }
  template <class D = DefaultDispatch, class... Args>
  decltype(auto) invoke(Args&&... args) const
      noexcept(HasNothrowInvocation<D, Args...>)
      requires(facade<F> && BasicTraits::template has_dispatch<D> &&
          requires { typename MatchedOverload<D, Args...>; }) {
    using dispatcher_type = typename details::overload_traits<
        MatchedOverload<D, Args...>>::dispatcher_type;
    const auto& dispatchers = static_cast<const typename Traits::meta*>(meta_)
        ->details::template dispatch_traits<D>::meta::dispatchers;
    auto dispatcher = std::get<dispatcher_type>(dispatchers);
    return dispatcher(ptr_, std::forward<Args>(args)...);
  }
  template <class... Args>
  decltype(auto) operator()(Args&&... args) const
      noexcept(HasNothrowInvocation<DefaultDispatch, Args...>)
      requires(facade<F> &&
          requires { typename MatchedOverload<DefaultDispatch, Args...>; })
      { return invoke(std::forward<Args>(args)...); }

 private:
  template <class P, class... Args>
  void initialize(Args&&... args) {
    new(ptr_) P(std::forward<Args>(args)...);
    meta_ = &Traits::template meta_storage<P>;
  }

  const typename BasicTraits::meta* meta_;
  alignas(F::constraints.max_align) char ptr_[F::constraints.max_size];
};

namespace details {

template <class T>
class sbo_ptr {
 public:
  template <class... Args>
  sbo_ptr(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
      requires(std::is_constructible_v<T, Args...>)
      : value_(std::forward<Args>(args)...) {}
  sbo_ptr(const sbo_ptr&) noexcept(std::is_nothrow_copy_constructible_v<T>)
      = default;
  sbo_ptr(sbo_ptr&&) noexcept(std::is_nothrow_move_constructible_v<T>)
      = default;

  T* operator->() const noexcept { return &value_; }

 private:
  mutable T value_;
};

template <class T>
class deep_ptr {
 public:
  template <class... Args>
  deep_ptr(Args&&... args) requires(std::is_constructible_v<T, Args...>)
      : ptr_(new T(std::forward<Args>(args)...)) {}
  deep_ptr(const deep_ptr& rhs) requires(std::is_copy_constructible_v<T>)
      : ptr_(rhs.ptr_ == nullptr ? nullptr : new T(*rhs.ptr_)) {}
  deep_ptr(deep_ptr&& rhs) noexcept : ptr_(rhs.ptr_) { rhs.ptr_ = nullptr; }
  ~deep_ptr() noexcept { delete ptr_; }

  T* operator->() const noexcept { return ptr_; }

 private:
  T* ptr_;
};

template <class F, class T, class... Args>
proxy<F> make_proxy_impl(Args&&... args) {
  return proxy<F>{std::in_place_type<
      std::conditional_t<proxiable<sbo_ptr<T>, F>, sbo_ptr<T>, deep_ptr<T>>>,
      std::forward<Args>(args)...};
}

}  // namespace details

template <class F, class T, class... Args>
proxy<F> make_proxy(Args&&... args)
    { return details::make_proxy_impl<F, T>(std::forward<Args>(args)...); }
template <class F, class T, class U, class... Args>
proxy<F> make_proxy(std::initializer_list<U> il, Args&&... args)
    { return details::make_proxy_impl<F, T>(il, std::forward<Args>(args)...); }
template <class F, class T>
proxy<F> make_proxy(T&& value) {
  return details::make_proxy_impl<F, std::decay_t<T>>(std::forward<T>(value));
}

// The following types and macros aim to simplify definition of dispatch and
// facade types prior to C++26
namespace details {

template <class Args>
struct overload_matching_helper {
  template <class O, class I> struct reduction : std::type_identity<O> {};
  template <class O, class I>
      requires(std::is_same_v<
          typename overload_traits<I>::forwarding_argument_types, Args>)
  struct reduction<O, I> : std::type_identity<I> {};
};
template <class Args, class... Os>
    requires(!std::is_void_v<recursive_reduction_t<
        overload_matching_helper<Args>::template reduction, void, Os...>>)
using matched_overload = recursive_reduction_t<
    overload_matching_helper<Args>::template reduction, void, Os...>;
template <class Args, class... Os>
constexpr bool matched_overload_is_noexcept =
    overload_traits<matched_overload<Args, Os...>>::is_noexcept;

template <class O, class I> struct flat_reduction : std::type_identity<O> {};
template <class... Os, class I> requires(!std::is_same_v<I, Os> && ...)
struct flat_reduction<std::tuple<Os...>, I>
    : std::type_identity<std::tuple<Os..., I>> {};
template <class... Os, class... Is>
struct flat_reduction<std::tuple<Os...>, std::tuple<Is...>>
    : recursive_reduction<flat_reduction, std::tuple<Os...>, Is...> {};
template <class O, class I>
struct overloads_reduction : std::type_identity<O> {};
template <class O, class I> requires(requires { typename I::overload_types; })
struct overloads_reduction<O, I>
    : flat_reduction<O, typename I::overload_types> {};

template <class... Os> requires(sizeof...(Os) > 0u)
struct dispatch_prototype { using overload_types = std::tuple<Os...>; };
template <class... Ds> requires(sizeof...(Ds) > 0u)
struct combined_dispatch_prototype : Ds... {
  using overload_types = recursive_reduction_t<
      overloads_reduction, std::tuple<>, Ds...>;
  using Ds::operator()...;
};
template <class Ds = std::tuple<>, proxiable_ptr_constraints C =
    relocatable_ptr_constraints, class R = void>
struct facade_prototype {
  using dispatch_types = typename flat_reduction<std::tuple<>, Ds>::type;
  static constexpr proxiable_ptr_constraints constraints = C;
  using reflection_type = R;
};

}  // namespace details

}  // namespace pro

#define PRO_DEF_MEMBER_DISPATCH(NAME, ...) \
    struct NAME : ::pro::details::dispatch_prototype<__VA_ARGS__> { \
      template <class __T, class... __Args> \
      decltype(auto) operator()(__T& __self, __Args&&... __args) \
          noexcept(::pro::details::matched_overload_is_noexcept< \
              std::tuple<__Args&&...>, __VA_ARGS__>) \
          requires( \
              requires{ \
                typename ::pro::details::matched_overload< \
                    std::tuple<__Args&&...>, __VA_ARGS__>; \
                __self.NAME(std::forward<__Args>(__args)...); \
              } && (!::pro::details::matched_overload_is_noexcept< \
                  std::tuple<__Args&&...>, __VA_ARGS__> || \
              noexcept(__self.NAME(std::forward<__Args>(__args)...)))) \
          { return __self.NAME(std::forward<__Args>(__args)...); } \
    }
#define PRO_DEF_FREE_DISPATCH(NAME, FUNC, ...) \
    struct NAME : ::pro::details::dispatch_prototype<__VA_ARGS__> { \
      template <class __T, class... __Args> \
      decltype(auto) operator()(__T& __self, __Args&&... __args) \
          noexcept(::pro::details::matched_overload_is_noexcept< \
              std::tuple<__Args&&...>, __VA_ARGS__>) \
          requires( \
              requires{ \
                typename ::pro::details::matched_overload< \
                    std::tuple<__Args&&...>, __VA_ARGS__>; \
                FUNC(__self, std::forward<__Args>(__args)...); \
              } && (!::pro::details::matched_overload_is_noexcept< \
                  std::tuple<__Args&&...>, __VA_ARGS__> || \
              noexcept(FUNC(__self, std::forward<__Args>(__args)...)))) \
          { return FUNC(__self, std::forward<__Args>(__args)...); } \
    }
#define PRO_DEF_COMBINED_DISPATCH(NAME, ...) \
    struct NAME : ::pro::details::combined_dispatch_prototype<__VA_ARGS__> {}
#define PRO_MAKE_DISPATCH_PACK(...) std::tuple<__VA_ARGS__>
#define PRO_DEF_FACADE(NAME, ...) \
    struct NAME : ::pro::details::facade_prototype<__VA_ARGS__> {}

#endif  // _MSFT_PROXY_
