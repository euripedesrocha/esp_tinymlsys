// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <span>
#include <system_error>
#include <type_traits>

#include "esp_execution/repeat.hpp"
#include "esp_execution/io/primitives.hpp"

namespace esp::execution::io {

namespace detail {

template <class IO>
concept member_queryable =
    requires(IO& object, std::span<const std::byte> command, std::span<std::byte> response) {
        { object.async_query(command, response) } -> sender;
    };

template <class IO>
concept query_endpoint = member_queryable<IO> || (member_readable<IO> && member_writable<IO>);

template <class Signature> struct exact_completion {
    using type = execution::completion_signatures<Signature>;
};

template <class Error> struct exact_completion<set_value_t(std::optional<Error>, std::size_t)> {
    using type = execution::completion_signatures<set_error_t(Error)>;
};

template <class Error> struct exact_completion<set_error_t(Error)> {
    using type = execution::completion_signatures<set_error_t(std::decay_t<Error>)>;
};

template <class Source, class Env>
concept exact_compatible =
    byte_count_sender_in<Source, Env> &&
    execution::detail::all_selected_completion_signatures_satisfy_v<
        completion_signatures_of_t<Source, Env>, set_error_t, []<class Error> {
            return std::constructible_from<std::decay_t<Error>, Error> &&
                   std::is_nothrow_move_constructible_v<std::decay_t<Error>>;
        }>;

template <class IO, class Buffer> class exact_sender {
    using primitive = std::conditional_t<std::is_const_v<typename Buffer::element_type>,
                                         async_write_some_t, async_read_some_t>;

    using primitive_type = decltype(primitive{}(std::declval<IO&>(), std::declval<Buffer>()));
    template <class Env>
        requires exact_compatible<primitive_type, Env>
    using signatures = execution::detail::completion_signatures_cat_t<
        execution::detail::map_completion_signatures_t<
            completion_signatures_of_t<primitive_type, Env>,
            []<class Tag, class... Arguments> {
                return std::type_identity<typename exact_completion<Tag(Arguments...)>::type>{};
            }>,
        execution::completion_signatures<set_value_t(), set_error_t(std::error_code),
                                         set_error_t(std::exception_ptr)>>;

    template <class Receiver> class operation {
        IO* object_;
        Buffer buffer_;
        Receiver receiver_;

        struct terminal_receiver {
            using receiver_concept = receiver_tag;
            operation* parent;

            decltype(auto) get_env() const noexcept {
                return execution::get_env(parent->receiver_);
            }
            void set_value() && noexcept { execution::set_value(std::move(parent->receiver_)); }
            void set_error(auto&& error) && noexcept {
                execution::set_error(std::move(parent->receiver_),
                                     std::forward<decltype(error)>(error));
            }
            void set_stopped() && noexcept { execution::set_stopped(std::move(parent->receiver_)); }
        };

        class step {
            template <class StepReceiver> class step_operation {
                operation* parent_;
                StepReceiver receiver_;

                struct primitive_receiver {
                    using receiver_concept = receiver_tag;
                    step_operation* parent;

                    decltype(auto) get_env() const noexcept {
                        return execution::get_env(parent->receiver_);
                    }
                    template <class Error>
                    void set_value(std::optional<Error> error, std::size_t count) && noexcept {
                        auto& remaining = parent->parent_->buffer_;
                        if (count > remaining.size() || (!error && !count)) {
                            execution::set_error(std::move(parent->receiver_),
                                                 std::make_error_code(std::errc::io_error));
                            return;
                        }
                        remaining = remaining.subspan(count);
                        if (error) {
                            execution::set_error(std::move(parent->receiver_), std::move(*error));
                            return;
                        }
                        execution::set_value(std::move(parent->receiver_));
                    }
                    void set_error(auto&& error) && noexcept {
                        using owned_error = std::decay_t<decltype(error)>;
                        auto complete =
                            [this, &error]() noexcept(
                                std::is_nothrow_constructible_v<owned_error, decltype(error)>) {
                                owned_error owned(std::forward<decltype(error)>(error));
                                execution::set_error(std::move(parent->receiver_),
                                                     std::move(owned));
                            };
                        if constexpr (noexcept(complete())) {
                            complete();
                        } else {
                            try {
                                complete();
                            } catch (...) {
                                execution::set_error(std::move(parent->receiver_),
                                                     std::current_exception());
                            }
                        }
                    }
                    void set_stopped() && noexcept {
                        execution::set_stopped(std::move(parent->receiver_));
                    }
                };

                using child_type = decltype(execution::connect(std::declval<primitive_type>(),
                                                               primitive_receiver{}));
                static constexpr bool connect_nothrow =
                    noexcept(primitive{}(std::declval<IO&>(), std::declval<Buffer>())) &&
                    noexcept(
                        execution::connect(std::declval<primitive_type>(), primitive_receiver{}));
                execution::detail::operation_state_storage<child_type> child_;

            public:
                using operation_state_concept = operation_state_tag;
                step_operation(operation* parent, StepReceiver receiver) noexcept(
                    std::is_nothrow_move_constructible_v<StepReceiver>)
                    : parent_(parent), receiver_(std::move(receiver)) {}
                step_operation(const step_operation&) =
                    delete ("the operation owns its receiver and child state");
                step_operation(step_operation&&) =
                    delete ("the primitive receiver retains this operation's address");

                void start() & noexcept {
                    auto make_child = [this]() noexcept(connect_nothrow) {
                        return execution::connect(primitive{}(*parent_->object_, parent_->buffer_),
                                                  primitive_receiver{this});
                    };
                    child_type* child;
                    if constexpr (connect_nothrow) {
                        child = &child_.emplace_from(make_child);
                    } else {
                        try {
                            child = &child_.emplace_from(make_child);
                        } catch (...) {
                            execution::set_error(std::move(receiver_), std::current_exception());
                            return;
                        }
                    }
                    execution::start(*child);
                }
            };

        public:
            using sender_concept = sender_tag;
            operation* parent;
            template <class Env> using completion_signatures = signatures<Env>;
            auto get_env() const noexcept { return empty_env{}; }
            auto connect(receiver auto receiver) const
                noexcept(noexcept(step_operation<decltype(receiver)>{parent,
                                                                     std::move(receiver)})) {
                return step_operation<decltype(receiver)>{parent, std::move(receiver)};
            }
        };

        struct finished {
            operation* parent;
            bool operator()() const noexcept { return parent->buffer_.empty(); }
        };
        using repeated_type = decltype(execution::repeat_until(step{}, finished{}));
        using child_type =
            decltype(execution::connect(std::declval<repeated_type>(), terminal_receiver{}));
        // The repeat factory only moves the step and predicate into its sender.
        static constexpr bool connect_nothrow =
            std::is_nothrow_move_constructible_v<step> &&
            std::is_nothrow_move_constructible_v<finished> &&
            noexcept(execution::connect(std::declval<repeated_type>(), terminal_receiver{}));
        execution::detail::operation_state_storage<child_type> child_;

    public:
        using operation_state_concept = operation_state_tag;
        operation(IO& object, Buffer buffer,
                  Receiver receiver) noexcept(std::is_nothrow_copy_constructible_v<Buffer> &&
                                              std::is_nothrow_move_constructible_v<Receiver>)
            : object_(&object), buffer_(buffer), receiver_(std::move(receiver)) {}
        operation(const operation&) = delete ("the operation owns its receiver and child state");
        operation(operation&&) = delete ("internal receivers retain this operation's address");

        void start() & noexcept {
            if (buffer_.empty()) {
                execution::set_value(std::move(receiver_));
                return;
            }
            auto make_child = [this]() noexcept(connect_nothrow) {
                return execution::connect(execution::repeat_until(step{this}, finished{this}),
                                          terminal_receiver{this});
            };
            child_type* child;
            if constexpr (connect_nothrow) {
                child = &child_.emplace_from(make_child);
            } else {
                try {
                    child = &child_.emplace_from(make_child);
                } catch (...) {
                    execution::set_error(std::move(receiver_), std::current_exception());
                    return;
                }
            }
            execution::start(*child);
        }
    };

public:
    using sender_concept = sender_tag;
    IO* object;
    Buffer buffer;
    template <class Env> using completion_signatures = signatures<Env>;
    auto get_env() const noexcept { return empty_env{}; }
    auto connect(execution::detail::receiver_for_sender<exact_sender> auto receiver) const
        noexcept(noexcept(operation<decltype(receiver)>{*object, buffer, std::move(receiver)})) {
        return operation<decltype(receiver)>{*object, buffer, std::move(receiver)};
    }
};

} // namespace detail

struct async_read_t {
    auto operator()(detail::member_readable auto& object,
                    std::span<std::byte> buffer) const noexcept {
        return detail::exact_sender{&object, buffer};
    }

    auto operator()(sender auto source, detail::member_readable auto& object) const {
        return execution::let_value(std::move(source),
                                    [&object](std::span<std::byte> buffer) noexcept {
                                        return async_read_t{}(object, buffer);
                                    });
    }

    auto operator()(detail::member_readable auto& object) const {
        return execution::detail::sender_adaptor_binding{
            [&object](auto source) { return async_read_t{}(std::move(source), object); }};
    }
};
inline constexpr async_read_t async_read{};

struct async_write_t {
    auto operator()(detail::member_writable auto& object,
                    std::span<const std::byte> buffer) const noexcept {
        return detail::exact_sender{&object, buffer};
    }

    auto operator()(sender auto source, detail::member_writable auto& object) const {
        return execution::let_value(std::move(source),
                                    [&object](std::span<const std::byte> buffer) noexcept {
                                        return async_write_t{}(object, buffer);
                                    });
    }

    auto operator()(detail::member_writable auto& object) const {
        return execution::detail::sender_adaptor_binding{
            [&object](auto source) { return async_write_t{}(std::move(source), object); }};
    }
};
inline constexpr async_write_t async_write{};

// Prefer the endpoint's native query; otherwise compose exact write then read.
// Success acknowledges the whole command and response. Transaction guarantees
// come from the endpoint; the composed fallback does not preserve bus ownership
// or repeated START. Native queries are submitted once, without generic retries.
struct async_query_t {
    auto operator()(sender auto source, detail::query_endpoint auto& object) const {
        if constexpr (detail::member_queryable<decltype(object)>) {
            return execution::let_value(
                std::move(source),
                [&object](
                    std::span<const std::byte> command,
                    std::span<std::byte>
                        response) noexcept(noexcept(auto(object.async_query(command, response)))) {
                    return object.async_query(command, response);
                });
        } else {
            return execution::let_value(
                std::move(source), [&object](std::span<const std::byte> command,
                                             std::span<std::byte> response) noexcept {
                    return execution::let_value(
                        io::async_write(object, command),
                        [&object, response] noexcept { return io::async_read(object, response); });
                });
        }
    }

    auto operator()(detail::query_endpoint auto& object, std::span<const std::byte> command,
                    std::span<std::byte> response) const {
        return (*this)(execution::just(command, response), object);
    }

    auto operator()(detail::query_endpoint auto& object) const {
        return execution::detail::sender_adaptor_binding{
            [&object](auto source) { return async_query_t{}(std::move(source), object); }};
    }
};
inline constexpr async_query_t async_query{};

} // namespace esp::execution::io
