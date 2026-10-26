// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <concepts>
#include <tuple>
#include <type_traits>
#include <variant>

#include <catch2/catch_test_macros.hpp>

#include "esp_execution/value_types.hpp"

namespace {
namespace execution = esp::execution;
using execution::completion_signatures;
using execution::set_error_t;
using execution::set_stopped_t;
using execution::set_value_t;
using execution::value_types_of_t;

template <class Signatures> struct typed_sender {
    using sender_concept = execution::sender_tag;
    template <class Env> using completion_signatures = Signatures;
};

template <class... Types> struct type_pack {};

template <class... Values> using value_signature = set_value_t(Values...);
template <class Type> using pair_array = Type[2];

template <class Sender, class Env = execution::empty_env,
          template <class...> class Tuple = execution::detail::decayed_tuple,
          template <class...> class Variant = execution::detail::variant_or_empty>
concept has_value_types = requires { typename value_types_of_t<Sender, Env, Tuple, Variant>; };

template <class Type>
    requires std::same_as<Type, int>
using only_int = Type;

struct alternate_env {};
struct environment_sender {
    using sender_concept = execution::sender_tag;
    template <class Env>
        requires std::same_as<Env, alternate_env>
    using completion_signatures = execution::completion_signatures<set_value_t(long)>;
};

struct throwing_copy {
    throwing_copy(const throwing_copy&) noexcept(false);
};
} // namespace

TEST_CASE("value_types_of_t decays value packs and keeps unique alternatives in order",
          "[signatures][value_types]") {
    using input = typed_sender<
        completion_signatures<set_value_t(int&), set_error_t(long), set_value_t(const int&),
                              set_value_t(float&, long&&), set_stopped_t(), set_value_t(int&)>>;
    STATIC_REQUIRE(std::same_as<value_types_of_t<input>,
                                std::variant<std::tuple<int>, std::tuple<float, long>>>);

    // decay also converts array/function references to pointers.
    using function = int(double);
    using array_and_function =
        typed_sender<completion_signatures<set_value_t(int (&)[3], function&)>>;
    STATIC_REQUIRE(std::same_as<value_types_of_t<array_and_function>,
                                std::variant<std::tuple<int*, function*>>>);
    using no_arguments = typed_sender<completion_signatures<set_value_t()>>;
    STATIC_REQUIRE(std::same_as<value_types_of_t<no_arguments>, std::variant<std::tuple<>>>);

    using stopped = typed_sender<completion_signatures<set_stopped_t()>>;
    using error = typed_sender<completion_signatures<set_error_t(int)>>;
    using never = typed_sender<completion_signatures<>>;
    STATIC_REQUIRE(std::same_as<value_types_of_t<stopped>, value_types_of_t<never>>);
    STATIC_REQUIRE(std::same_as<value_types_of_t<error>, value_types_of_t<never>>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<value_types_of_t<never>>);
}

TEST_CASE("value_types_of_t lets callers choose tuple and collection representations",
          "[signatures][value_types]") {
    using input = typed_sender<
        completion_signatures<set_value_t(int&), set_value_t(const int&), set_value_t(int&)>>;
    // A custom collector receives every transformed value pack, including duplicates.
    STATIC_REQUIRE(std::same_as<value_types_of_t<input, execution::empty_env,
                                                 execution::detail::decayed_tuple, type_pack>,
                                type_pack<std::tuple<int>, std::tuple<int>, std::tuple<int>>>);
    STATIC_REQUIRE(std::same_as<value_types_of_t<input, execution::empty_env, std::tuple>,
                                std::variant<std::tuple<int&>, std::tuple<const int&>>>);

    // With a custom Tuple, the default collection must still decay and deduplicate.
    using function = int(double);
    using alternatives =
        typed_sender<completion_signatures<set_value_t(int (&)[3]), set_value_t(function&),
                                           set_value_t(int (&)[5])>>;
    STATIC_REQUIRE(
        std::same_as<value_types_of_t<alternatives, execution::empty_env, std::type_identity_t>,
                     std::variant<int*, function*>>);

    using empty = typed_sender<completion_signatures<set_error_t(float), set_stopped_t()>>;
    STATIC_REQUIRE(std::same_as<value_types_of_t<empty, execution::empty_env, only_int, type_pack>,
                                type_pack<>>);
}

TEST_CASE("value_types_of_t uses the supplied environment and ignores other channels",
          "[signatures][value_types]") {
    // This sender has no signatures in empty_env: both the constraint and query
    // must use the caller's environment.
    STATIC_REQUIRE(std::same_as<value_types_of_t<environment_sender, alternate_env>,
                                std::variant<std::tuple<long>>>);
    using mixed =
        typed_sender<completion_signatures<set_error_t(float), set_value_t(int), set_stopped_t()>>;
    STATIC_REQUIRE(
        std::same_as<value_types_of_t<mixed, execution::empty_env, only_int, std::type_identity_t>,
                     int>);
    // Gathering types imposes no consumer-specific storage or exception policy.
    using throwing_value = typed_sender<completion_signatures<set_value_t(throwing_copy&)>>;
    STATIC_REQUIRE(
        std::same_as<value_types_of_t<throwing_value>, std::variant<std::tuple<throwing_copy>>>);
}

TEST_CASE("value_types_of_t preserves arbitrary custom collection result types",
          "[signatures][value_types]") {
    using one = typed_sender<completion_signatures<set_value_t(int)>>;
    // P2300R10's sender-in-of uses this exact function-signature representation.
    STATIC_REQUIRE(
        std::same_as<
            value_types_of_t<one, execution::empty_env, value_signature, std::type_identity_t>,
            set_value_t(int)>);
    STATIC_REQUIRE(std::same_as<
                   value_types_of_t<one, execution::empty_env, std::type_identity_t, std::add_cv_t>,
                   const volatile int>);
    STATIC_REQUIRE(
        std::same_as<value_types_of_t<one, execution::empty_env, std::type_identity_t, pair_array>,
                     int[2]>);
}

TEST_CASE("value_types_of_t rejects unavailable tuple and collection customizations cleanly",
          "[signatures][value_types][constraints]") {
    using no_arguments = typed_sender<completion_signatures<set_value_t()>>;
    STATIC_REQUIRE_FALSE(has_value_types<no_arguments, execution::empty_env, std::type_identity_t>);
    using multiple_arguments = typed_sender<completion_signatures<set_value_t(int, long)>>;
    STATIC_REQUIRE_FALSE(
        has_value_types<multiple_arguments, execution::empty_env, std::type_identity_t>);
    using multiple_values =
        typed_sender<completion_signatures<set_value_t(int), set_value_t(long)>>;
    STATIC_REQUIRE_FALSE(
        has_value_types<multiple_values, execution::empty_env, std::tuple, std::type_identity_t>);
    using no_values = typed_sender<completion_signatures<>>;
    STATIC_REQUIRE_FALSE(
        has_value_types<no_values, execution::empty_env, std::tuple, std::type_identity_t>);
    using selected_invalid = typed_sender<completion_signatures<set_value_t(long)>>;
    STATIC_REQUIRE_FALSE(has_value_types<selected_invalid, execution::empty_env, only_int>);
}
