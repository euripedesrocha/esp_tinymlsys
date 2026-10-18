// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_execution/connect.hpp"
#include "esp_execution/detail/completion_signatures.hpp"
#include "esp_execution/detail/operation_state_storage.hpp"
#include "esp_execution/environment.hpp"
#include "esp_execution/operation_state.hpp"
#include "esp_execution/sender_adaptor.hpp"

namespace esp::execution {
namespace detail::repeat {

template <class Sender, class Receiver>
concept lvalue_member_connectable = requires(Sender& source, Receiver&& receiver) {
    { source.connect(std::forward<Receiver>(receiver)) } -> operation_state;
};

// Borrows the predecessor owned by the parent operation; copying copies only its address.
template <execution::sender Sender> class sender_reference {
public:
    using sender_concept = sender_tag;

    template <class Environment>
        requires sender_in<Sender, Environment>
    using completion_signatures = completion_signatures_of_t<Sender, Environment>;

    constexpr explicit sender_reference(Sender& source) noexcept
        : source_(std::addressof(source)) {}

    sender_reference(Sender&&) = delete ("a sender reference must borrow an existing lvalue");

    constexpr decltype(auto) get_env() const noexcept {
        return execution::get_env(std::as_const(*source_));
    }

    constexpr auto connect(receiver auto&& receiver) const
        noexcept(noexcept(source_->connect(std::forward<decltype(receiver)>(receiver))))
        requires lvalue_member_connectable<Sender, decltype(receiver)>
    {
        return source_->connect(std::forward<decltype(receiver)>(receiver));
    }

private:
    Sender* source_;
};

template <class Environment> struct connect_probe_receiver {
    using receiver_concept = receiver_tag;

    constexpr Environment get_env() const noexcept { std::unreachable(); }

    constexpr void set_value(auto&&...) && noexcept {}

    constexpr void set_error(auto&&) && noexcept {}

    constexpr void set_stopped() && noexcept {}
};

template <class Sender, class Environment = empty_env>
concept lvalue_connectable = requires(std::remove_cvref_t<Sender>& sender) {
    execution::connect(sender_reference{sender}, connect_probe_receiver<Environment>{});
};

template <class Error>
concept storable_error =
    std::same_as<Error, std::decay_t<Error>> && std::is_nothrow_move_constructible_v<Error> &&
    std::is_nothrow_destructible_v<Error>;

template <class Signature> inline constexpr bool storable_error_signature_v = true;

template <class Error>
inline constexpr bool storable_error_signature_v<set_error_t(Error)> = storable_error<Error>;

template <class Sender, class Environment = empty_env>
concept compatible =
    sender_in<Sender, Environment> && lvalue_connectable<Sender, Environment> &&
    all_completion_signatures_satisfy_v<completion_signatures_of_t<Sender, Environment>,
                                        []<class Signature> {
                                            return storable_error_signature_v<Signature>;
                                        }> &&
    count_completion_signatures_v<completion_signatures_of_t<Sender, Environment>, set_value_t> !=
        0;

template <class Control, class Signature> inline constexpr bool control_accepts_signature_v = true;

template <class Control, class... Arguments>
inline constexpr bool control_accepts_signature_v<Control, set_value_t(Arguments...)> =
    requires(Control& control) {
        {
            control.complete_after_value(std::declval<Arguments>()...)
        } noexcept -> std::same_as<bool>;
    };

template <class Control, class Sender, class Environment = empty_env>
concept control_for =
    sender_in<Sender, Environment> && std::move_constructible<Control> &&
    requires(const Control& control) {
        typename std::bool_constant<Control::can_complete>;
        { control.complete_before_start() } noexcept -> std::same_as<bool>;
    } &&
    all_completion_signatures_satisfy_v<completion_signatures_of_t<Sender, Environment>,
                                        []<class Signature> {
                                            return control_accepts_signature_v<Control, Signature>;
                                        }>;

template <class Sender, class Control, class Receiver>
concept operation_compatible = receiver<Receiver> && compatible<Sender, env_of_t<Receiver>> &&
                               control_for<Control, Sender, env_of_t<Receiver>>;

template <class Signature> inline constexpr std::size_t error_storage_size_v = 1;

template <class Error>
inline constexpr std::size_t error_storage_size_v<set_error_t(Error)> = sizeof(Error);

template <class Signature> inline constexpr std::size_t error_storage_alignment_v = 1;

template <class Error>
inline constexpr std::size_t error_storage_alignment_v<set_error_t(Error)> = alignof(Error);

template <class Error>
concept storable_error_argument = storable_error<std::decay_t<Error>> &&
                                  std::is_nothrow_constructible_v<std::decay_t<Error>, Error&&>;

template <class Error, class Storage>
concept error_storable_in = requires(Storage& storage, Error&& error) {
    { storage.emplace(std::forward<Error>(error)) } noexcept -> std::same_as<void>;
};

template <class Receiver, class Signatures> class pending_error {
public:
    pending_error() = default;
    pending_error(const pending_error&) =
        delete ("pending error storage belongs to one operation state");
    pending_error(pending_error&&) =
        delete ("pending error storage belongs to one operation state");
    pending_error& operator=(const pending_error&) =
        delete ("pending error storage belongs to one operation state");
    pending_error&
    operator=(pending_error&&) = delete ("pending error storage belongs to one operation state");

    template <class Error>
    void emplace(Error&&) noexcept = delete ("the sender advertises no error completions");

    void complete(Receiver&) noexcept { std::unreachable(); }
};

template <class Receiver, class... Signatures>
    requires(count_completion_signatures_v<completion_signatures<Signatures...>, set_error_t> != 0)
class pending_error<Receiver, completion_signatures<Signatures...>> {
    static constexpr std::size_t storage_size = std::max({error_storage_size_v<Signatures>...});
    static constexpr std::size_t storage_alignment =
        std::max({error_storage_alignment_v<Signatures>...});

public:
    pending_error() = default;
    pending_error(const pending_error&) =
        delete ("pending error storage uniquely owns its active error");
    pending_error(pending_error&&) =
        delete ("pending error storage retains its error in an inline buffer");
    pending_error& operator=(const pending_error&) =
        delete ("pending error storage uniquely owns its active error");
    pending_error& operator=(pending_error&&) =
        delete ("pending error storage retains its error in an inline buffer");

    ~pending_error() { reset(); }

    template <storable_error_argument Error>
        requires(std::same_as<set_error_t(std::decay_t<Error>), Signatures> || ...)
    void emplace(Error&& error) noexcept {
        using stored_error = std::decay_t<Error>;

        reset();
        ::new (static_cast<void*>(storage_)) stored_error(std::forward<Error>(error));
        destroy_ = [](void* storage) noexcept {
            std::destroy_at(std::launder(reinterpret_cast<stored_error*>(storage)));
        };
        complete_ = [](void* storage, Receiver& receiver) noexcept {
            auto* stored = std::launder(reinterpret_cast<stored_error*>(storage));
            stored_error error(std::move(*stored));
            std::destroy_at(stored);
            execution::set_error(std::move(receiver), std::move(error));
        };
    }

    void complete(Receiver& receiver) noexcept {
        contract_assert(destroy_ != nullptr);
        contract_assert(complete_ != nullptr);
        auto complete = complete_;
        destroy_ = nullptr;
        complete_ = nullptr;
        complete(storage_, receiver);
    }

private:
    void reset() noexcept {
        if (destroy_ != nullptr) {
            destroy_(storage_);
            destroy_ = nullptr;
        }
        complete_ = nullptr;
    }

    alignas(storage_alignment) std::byte storage_[storage_size];
    void (*destroy_)(void*) noexcept = nullptr;
    void (*complete_)(void*, Receiver&) noexcept = nullptr;
};

struct forever_control {
    static constexpr bool can_complete = false;

    constexpr bool complete_before_start() const noexcept { return false; }

    constexpr bool complete_after_value(auto&&...) noexcept { return false; }
};

class count_control {
public:
    static constexpr bool can_complete = true;

    constexpr explicit count_control(std::size_t remaining) noexcept : remaining_(remaining) {}

    constexpr bool complete_before_start() const noexcept { return remaining_ == 0; }

    constexpr bool complete_after_value(auto&&...) noexcept { return --remaining_ == 0; }

private:
    std::size_t remaining_;
};

template <class Predicate> class predicate_control {
public:
    static constexpr bool can_complete = true;

    constexpr explicit predicate_control(Predicate predicate) noexcept(
        std::is_nothrow_move_constructible_v<Predicate>)
        : predicate_(std::move(predicate)) {}

    constexpr bool complete_before_start() const noexcept { return false; }

    constexpr bool complete_after_value(auto&&... values) noexcept
        requires std::is_nothrow_invocable_r_v<bool, Predicate&, decltype(values)...>
    {
        return std::invoke(predicate_, std::forward<decltype(values)>(values)...);
    }

private:
    Predicate predicate_;
};

template <class Sender, class Control>
    requires control_for<Control, Sender>
class sender {
public:
    using sender_concept = sender_tag;

    template <class Environment>
        requires control_for<Control, Sender, Environment>
    using completion_signatures = completion_signatures_cat_t<
        std::conditional_t<Control::can_complete, execution::completion_signatures<set_value_t()>,
                           execution::completion_signatures<>>,
        remove_completion_signatures_for_t<completion_signatures_of_t<Sender, Environment>,
                                           set_value_t>>;

    constexpr sender(Sender source,
                     Control control) noexcept(std::is_nothrow_move_constructible_v<Sender> &&
                                               std::is_nothrow_move_constructible_v<Control>)
        : source_(std::move(source)), control_(std::move(control)) {}

    constexpr decltype(auto) get_env() const noexcept(noexcept(execution::get_env(source_))) {
        return execution::get_env(source_);
    }

    template <class Receiver>
        requires operation_compatible<Sender, Control, Receiver>
    class operation {
        Sender source_;
        Receiver receiver_;
        Control control_;

        using child_signatures = completion_signatures_of_t<Sender, env_of_t<Receiver>>;
        using error_storage = pending_error<Receiver, child_signatures>;

        class repeat_receiver {
        public:
            using receiver_concept = receiver_tag;

            constexpr explicit repeat_receiver(operation* parent) noexcept : parent_(parent) {}

            constexpr decltype(auto) get_env() const
                noexcept(noexcept(execution::get_env(std::declval<const Receiver&>()))) {
                return execution::get_env(parent_->receiver_);
            }

            constexpr void set_value(auto&&... values) && noexcept {
                parent_->complete_iteration(std::forward<decltype(values)>(values)...);
            }

            constexpr void set_error(auto&& error) && noexcept
                requires error_storable_in<decltype(error), error_storage>
            {
                parent_->complete_error(std::forward<decltype(error)>(error));
            }

            constexpr void set_stopped() && noexcept { parent_->complete_stopped(); }

        private:
            operation* parent_;
        };

        using child_operation = decltype(execution::connect(
            std::declval<sender_reference<Sender>>(), std::declval<repeat_receiver>()));

        using request_type = std::uint8_t;
        static constexpr request_type processing = 1U << 0;
        static constexpr request_type iteration_pending = 1U << 1;
        static constexpr request_type value_completed = 1U << 2;
        static constexpr request_type error_completed = 1U << 3;
        static constexpr request_type stopped_completed = 1U << 4;
        static constexpr request_type terminal_completion =
            value_completed | error_completed | stopped_completed;

    public:
        using operation_state_concept = operation_state_tag;

        constexpr operation(Sender source, Receiver receiver, Control control) noexcept(
            std::is_nothrow_move_constructible_v<Sender> &&
            std::is_nothrow_move_constructible_v<Receiver> &&
            std::is_nothrow_move_constructible_v<Control>)
            : source_(std::move(source)), receiver_(std::move(receiver)),
              control_(std::move(control)) {}

        operation(const operation&) =
            delete ("an operation state uniquely owns its receiver and child operation");
        operation& operator=(const operation&) =
            delete ("an operation state uniquely owns its receiver and child operation");
        operation(operation&&) =
            delete ("the child receiver retains the operation state's stable address");
        operation& operator=(operation&&) =
            delete ("the child receiver retains the operation state's stable address");

        void start() & noexcept {
            if (control_.complete_before_start()) {
                request(value_completed);
                return;
            }
            request(iteration_pending);
        }

    private:
        void request(request_type event) noexcept {
            const auto previous =
                requested_.fetch_or(event | processing, std::memory_order_acq_rel);
            if ((previous & processing) == 0) {
                process_pending();
            }
        }

        void process_pending() noexcept {
            while (true) {
                const auto pending_requests = requested_.load(std::memory_order_acquire);
                if ((pending_requests & terminal_completion) != 0) {
                    deliver_terminal(pending_requests);
                    return;
                }

                if ((pending_requests & iteration_pending) != 0) {
                    requested_.fetch_and(static_cast<request_type>(~iteration_pending),
                                         std::memory_order_acq_rel);
                    execution::start(child_.emplace_from([this] {
                        return execution::connect(sender_reference{source_}, repeat_receiver{this});
                    }));
                    continue;
                }

                auto expected = processing;
                if (requested_.compare_exchange_weak(expected, 0, std::memory_order_release,
                                                     std::memory_order_acquire)) {
                    return;
                }
            }
        }

        void complete_iteration(auto&&... values) noexcept {
            if (control_.complete_after_value(std::forward<decltype(values)>(values)...)) {
                request(value_completed);
            } else {
                request(iteration_pending);
            }
        }

        void complete_error(auto&& error) noexcept {
            error_.emplace(std::forward<decltype(error)>(error));
            request(error_completed);
        }

        void complete_stopped() noexcept { request(stopped_completed); }

        void deliver_terminal(request_type state) noexcept {
            if constexpr (Control::can_complete) {
                if ((state & value_completed) != 0) {
                    execution::set_value(std::move(receiver_));
                    return;
                }
            }
            if constexpr (count_completion_signatures_v<child_signatures, set_error_t> != 0) {
                if ((state & error_completed) != 0) {
                    error_.complete(receiver_);
                    return;
                }
            }
            if constexpr (count_completion_signatures_v<child_signatures, set_stopped_t> != 0) {
                if ((state & stopped_completed) != 0) {
                    execution::set_stopped(std::move(receiver_));
                    return;
                }
            }
            std::unreachable();
        }

        operation_state_storage<child_operation> child_;
        [[no_unique_address]] error_storage error_;
        std::atomic<request_type> requested_{0};
    };

    template <class Receiver>
        requires operation_compatible<Sender, Control, Receiver>
    constexpr auto connect(Receiver receiver) && {
        return operation<Receiver>{std::move(source_), std::move(receiver), std::move(control_)};
    }

    template <class Receiver>
        requires std::copy_constructible<Sender> && std::copy_constructible<Control> &&
                 operation_compatible<Sender, Control, Receiver>
    constexpr auto connect(Receiver receiver) const& {
        return operation<Receiver>{source_, std::move(receiver), control_};
    }

private:
    Sender source_;
    Control control_;
};

template <class Sender, class Control>
constexpr auto make_sender(Sender&& source, Control&& control) {
    return sender<std::decay_t<Sender>, std::decay_t<Control>>{std::forward<Sender>(source),
                                                               std::forward<Control>(control)};
}

} // namespace detail::repeat

struct repeat_t {
    template <sender Sender>
        requires detail::repeat::compatible<Sender>
    constexpr auto operator()(Sender&& source) const {
        return detail::repeat::make_sender(std::forward<Sender>(source),
                                           detail::repeat::forever_control{});
    }

    constexpr auto operator()() const {
        return detail::sender_adaptor_binding{std::bind_back(*this)};
    }
};

struct repeat_until_t {
    template <sender Sender, detail::movable_value Predicate>
        requires detail::repeat::compatible<Sender> &&
                 detail::repeat::control_for<
                     detail::repeat::predicate_control<std::decay_t<Predicate>>, Sender>
    constexpr auto operator()(Sender&& source, Predicate&& predicate) const {
        using predicate_type = std::decay_t<Predicate>;
        return detail::repeat::make_sender(
            std::forward<Sender>(source),
            detail::repeat::predicate_control<predicate_type>{std::forward<Predicate>(predicate)});
    }

    template <detail::movable_value Predicate>
    constexpr auto operator()(Predicate&& predicate) const {
        return detail::sender_adaptor_binding{
            std::bind_back(*this, std::forward<Predicate>(predicate))};
    }
};

struct repeat_n_t {
    template <sender Sender>
        requires detail::repeat::compatible<Sender>
    constexpr auto operator()(Sender&& source, std::size_t count) const {
        return detail::repeat::make_sender(std::forward<Sender>(source),
                                           detail::repeat::count_control{count});
    }

    constexpr auto operator()(std::size_t count) const {
        return detail::sender_adaptor_binding{std::bind_back(*this, count)};
    }
};

inline constexpr repeat_t repeat{};
inline constexpr repeat_until_t repeat_until{};
inline constexpr repeat_n_t repeat_n{};

} // namespace esp::execution
