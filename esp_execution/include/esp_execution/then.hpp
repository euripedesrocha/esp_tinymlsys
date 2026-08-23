// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_execution/connect.hpp"
#include "esp_execution/environment.hpp"
#include "esp_execution/sender_adaptor.hpp"

namespace esp::execution {

namespace detail::then {

// Callable eligibility for every value completion advertised by the sender.
template <class Signature, class Callable> inline constexpr bool nothrow_signature_v = true;

template <class... Values, class Callable>
inline constexpr bool nothrow_signature_v<set_value_t(Values...), Callable> =
    std::is_nothrow_invocable_v<Callable&&, Values...>;

template <class Signatures, class Callable> inline constexpr bool nothrow_invocable_v = false;

template <class... Signatures, class Callable>
inline constexpr bool nothrow_invocable_v<completion_signatures<Signatures...>, Callable> =
    (nothrow_signature_v<Signatures, Callable> && ...);

template <class Signatures, class Callable>
concept nothrow_invocable = nothrow_invocable_v<Signatures, Callable>;

// One-to-one completion-signature transformation. Error and stopped
// completions pass through unchanged; value completions become callable results.
template <class Result> struct result_signature {
    using type = set_value_t(Result);
};

template <> struct result_signature<void> {
    using type = set_value_t();
};

template <class Signature, class Callable> struct signature {
    using type = Signature;
};

template <class... Values, class Callable>
    requires std::is_nothrow_invocable_v<Callable&&, Values...>
struct signature<set_value_t(Values...), Callable> {
private:
    using result_type = std::invoke_result_t<Callable&&, Values...>;

public:
    using type = typename result_signature<result_type>::type;
};

template <class Signatures, class Callable> struct completion_signatures_transform;

template <class... Signatures, class Callable>
struct completion_signatures_transform<completion_signatures<Signatures...>, Callable> {
    using type = completion_signatures<typename signature<Signatures, Callable>::type...>;
};

template <class Signatures, class Callable>
using completion_signatures_transform_t =
    typename completion_signatures_transform<Signatures, Callable>::type;

template <class Receiver, class Callable> class receiver_adaptor {
public:
    using receiver_concept = receiver_tag;

    constexpr receiver_adaptor(Receiver receiver, Callable callable)
        : receiver_(std::move(receiver)), callable_(std::move(callable)) {}

    constexpr decltype(auto) get_env() const noexcept { return execution::get_env(receiver_); }

    constexpr void set_value(auto&&... values) && noexcept
        requires std::is_nothrow_invocable_v<Callable&&, decltype(values)...>
    {
        if constexpr (std::is_void_v<std::invoke_result_t<Callable&&, decltype(values)...>>) {
            std::invoke(std::move(callable_), std::forward<decltype(values)>(values)...);
            execution::set_value(std::move(receiver_));
        } else {
            execution::set_value(
                std::move(receiver_),
                std::invoke(std::move(callable_), std::forward<decltype(values)>(values)...));
        }
    }

    constexpr void set_error(auto&& error) && noexcept {
        execution::set_error(std::move(receiver_), std::forward<decltype(error)>(error));
    }

    constexpr void set_stopped() && noexcept { execution::set_stopped(std::move(receiver_)); }

private:
    Receiver receiver_;
    Callable callable_;
};

template <class Sender, class Callable> class sender {
public:
    using sender_concept = sender_tag;

    template <class Env>
    using completion_signatures =
        completion_signatures_transform_t<completion_signatures_of_t<Sender, Env>, Callable>;

    constexpr sender(Sender sender, Callable callable)
        : sender_(std::move(sender)), callable_(std::move(callable)) {}

    constexpr decltype(auto) get_env() const noexcept { return execution::get_env(sender_); }

    constexpr auto connect(receiver auto&& receiver) && {
        using receiver_type = std::decay_t<decltype(receiver)>;
        return execution::connect(
            std::move(sender_),
            receiver_adaptor<receiver_type, Callable>{std::forward<decltype(receiver)>(receiver),
                                                      std::move(callable_)});
    }

    constexpr auto connect(receiver auto&& receiver) const&
        requires std::copy_constructible<Sender> && std::copy_constructible<Callable>
    {
        using receiver_type = std::decay_t<decltype(receiver)>;
        return execution::connect(sender_,
                                  receiver_adaptor<receiver_type, Callable>{
                                      std::forward<decltype(receiver)>(receiver), callable_});
    }

private:
    Sender sender_;
    Callable callable_;
};

} // namespace detail::then

// Public adaptor customization object.
struct then_t {
    constexpr auto operator()(sender_in auto&& sender, detail::movable_value auto&& callable) const
        requires detail::then::nothrow_invocable<completion_signatures_of_t<decltype(sender)>,
                                                 std::decay_t<decltype(callable)>>
    {
        return detail::then::sender<std::decay_t<decltype(sender)>,
                                    std::decay_t<decltype(callable)>>{
            std::forward<decltype(sender)>(sender), std::forward<decltype(callable)>(callable)};
    }

    constexpr auto operator()(detail::movable_value auto&& callable) const {
        return detail::sender_adaptor_binding{
            std::bind_back(*this, std::forward<decltype(callable)>(callable))};
    }
};

inline constexpr then_t then{};

} // namespace esp::execution
