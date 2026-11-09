// SPDX-License-Identifier: Apache-2.0
#include "esp_execution.hpp"
#include "esp_execution/io/operations.hpp"
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <system_error>
#include <algorithm>
#include <array>
#include <utility>
namespace {
namespace ex = esp::execution;

enum class native_error { failed, timeout };
struct move_only_error {
    int code;
    explicit move_only_error(int code) noexcept : code(code) {}
    move_only_error(move_only_error&&) noexcept = default;
    move_only_error(const move_only_error&) = delete;
};

struct typed_output {
    int values = 0, errors = 0, code = 0;
};
struct typed_receiver {
    using receiver_concept = ex::receiver_tag;
    typed_output* out;
    void set_value() && noexcept { ++out->values; }
    void set_error(native_error error) && noexcept {
        ++out->errors;
        CHECK(error == native_error::failed);
    }
    void set_error(move_only_error error) && noexcept {
        ++out->errors;
        out->code = error.code;
    }
    void set_error(int error) && noexcept {
        ++out->errors;
        out->code = error;
    }
    void set_error(std::error_code) && noexcept { FAIL_CHECK("Unexpected facade error"); }
    void set_error(std::exception_ptr) && noexcept { FAIL_CHECK("Unexpected exception"); }
    void set_stopped() && noexcept { FAIL_CHECK("Unexpected stop"); }
};

// An endpoint need not name an error_type, and one operation can advertise
// different typed conditions as well as an upstream execution error.
struct choice_endpoint {
    enum class outcome { success, native_condition, move_only_condition, upstream_error };
    outcome completion;
    unsigned calls = 0;
    struct source {
        using sender_concept = ex::sender_tag;
        choice_endpoint* endpoint;
        template <class Env>
        using completion_signatures =
            ex::completion_signatures<ex::set_value_t(std::optional<native_error>, std::size_t),
                                      ex::set_value_t(std::optional<move_only_error>, std::size_t),
                                      ex::set_error_t(int)>;
        template <class Receiver> struct operation {
            using operation_state_concept = ex::operation_state_tag;
            choice_endpoint* endpoint;
            Receiver receiver;
            void start() & noexcept {
                ++endpoint->calls;
                switch (endpoint->completion) {
                case outcome::success:
                    ex::set_value(std::move(receiver), std::optional<native_error>{},
                                  std::size_t{1});
                    break;
                case outcome::native_condition:
                    ex::set_value(std::move(receiver), std::optional{native_error::failed},
                                  std::size_t{1});
                    break;
                case outcome::move_only_condition:
                    ex::set_value(std::move(receiver),
                                  std::optional<move_only_error>{std::in_place, 17},
                                  std::size_t{1});
                    break;
                case outcome::upstream_error:
                    ex::set_error(std::move(receiver), 42);
                }
            }
        };
        auto connect(ex::receiver auto receiver) const noexcept {
            return operation<decltype(receiver)>{endpoint, std::move(receiver)};
        }
    };
    auto async_read_some(std::span<std::byte>) noexcept { return source{this}; }
};

template <class Error> struct borrowed_error_endpoint {
    Error error;
    bool fail = false, deferred = false;
    unsigned calls = 0;
    void* pending = nullptr;
    void (*complete_pending)(void*) noexcept = nullptr;
    struct source {
        using sender_concept = ex::sender_tag;
        borrowed_error_endpoint* endpoint;
        std::size_t size;
        template <class Env>
        using completion_signatures =
            ex::completion_signatures<ex::set_value_t(std::optional<Error>, std::size_t),
                                      ex::set_error_t(const Error&)>;
        template <class Receiver> struct operation {
            using operation_state_concept = ex::operation_state_tag;
            borrowed_error_endpoint* endpoint;
            std::size_t size;
            Receiver receiver;
            void complete() noexcept {
                if (endpoint->fail)
                    ex::set_error(std::move(receiver), std::as_const(endpoint->error));
                else
                    ex::set_value(std::move(receiver), std::optional<Error>{},
                                  std::min(size, std::size_t{1}));
            }
            void start() & noexcept {
                ++endpoint->calls;
                if (endpoint->deferred) {
                    CHECK(endpoint->pending == nullptr);
                    endpoint->pending = this;
                    endpoint->complete_pending = [](void* pending) noexcept {
                        static_cast<operation*>(pending)->complete();
                    };
                } else {
                    complete();
                }
            }
        };
        auto connect(ex::receiver auto receiver) const noexcept {
            return operation<decltype(receiver)>{endpoint, size, std::move(receiver)};
        }
    };
    auto async_read_some(std::span<std::byte> buffer) noexcept {
        return source{this, buffer.size()};
    }
    auto async_write_some(std::span<const std::byte> buffer) noexcept {
        return source{this, buffer.size()};
    }
};

struct copied_error {
    int code;
    bool throws;
    copied_error(int code, bool throws) noexcept : code(code), throws(throws) {}
    copied_error(const copied_error& other) : code(other.code), throws(other.throws) {
        if (throws)
            throw 47;
    }
    copied_error(copied_error&&) noexcept = default;
};

struct independent_state {
    std::array<std::byte, 8> input{}, output{};
    std::array<char, 32> trace{};
    std::size_t read_position = 0, write_position = 0, trace_size = 0, chunk = 2;
    bool write_error = false, read_error = false, stop_write = false, stop_read = false,
         deferred = false;
    std::size_t error_count = 0;
    void* pending = nullptr;
    void (*complete_pending)(void*) noexcept = nullptr;
};
struct independent_endpoint {
    independent_state& state;
    auto transfer(bool write, std::span<const std::byte> command,
                  std::span<std::byte> response) noexcept
        -> std::pair<std::optional<std::error_code>, std::size_t> {
        state.trace[state.trace_size++] = write ? 'w' : 'r';
        const bool failed = write ? state.write_error : state.read_error;
        const auto count = std::min(failed ? state.error_count : state.chunk,
                                    write ? command.size() : response.size());
        if (write) {
            std::copy_n(command.begin(), count, state.output.begin() + state.write_position);
            state.write_position += count;
        } else {
            std::copy_n(state.input.begin() + state.read_position, count, response.begin());
            state.read_position += count;
        }
        return {failed ? std::optional{std::make_error_code(std::errc::io_error)} : std::nullopt,
                count};
    }
    struct source {
        using sender_concept = ex::sender_tag;
        independent_endpoint* endpoint;
        bool write;
        std::span<const std::byte> command;
        std::span<std::byte> response;
        template <class Env>
        using completion_signatures =
            ex::completion_signatures<ex::set_value_t(std::optional<std::error_code>, std::size_t),
                                      ex::set_stopped_t()>;
        auto get_env() const noexcept { return ex::empty_env{}; }
        template <class Receiver> struct operation {
            using operation_state_concept = ex::operation_state_tag;
            independent_endpoint* endpoint;
            bool write;
            std::span<const std::byte> command;
            std::span<std::byte> response;
            Receiver receiver;
            void complete() noexcept {
                auto& state = endpoint->state;
                if (write ? state.stop_write : state.stop_read) {
                    ex::set_stopped(std::move(receiver));
                    return;
                }
                auto [error, count] = endpoint->transfer(write, command, response);
                ex::set_value(std::move(receiver), error, count);
            }
            void start() & noexcept {
                auto& state = endpoint->state;
                if (state.deferred) {
                    CHECK(!state.pending);
                    state.pending = this;
                    state.complete_pending = [](void* pending) noexcept {
                        static_cast<operation*>(pending)->complete();
                    };
                } else {
                    complete();
                }
            }
        };
        auto connect(ex::receiver auto receiver) const {
            return operation<decltype(receiver)>{endpoint, write, command, response,
                                                 std::move(receiver)};
        }
    };
    auto async_read_some(std::span<std::byte> buffer) { return source{this, false, {}, buffer}; }
    auto async_write_some(std::span<const std::byte> buffer) {
        return source{this, true, buffer, {}};
    }
};
struct async_input {
    independent_endpoint& endpoint;
    auto async_read_some(std::span<std::byte> buffer) { return endpoint.async_read_some(buffer); }
};
struct async_output {
    independent_endpoint& endpoint;
    auto async_write_some(std::span<const std::byte> buffer) {
        return endpoint.async_write_some(buffer);
    }
};
struct sequence_output {
    unsigned values = 0, errors = 0, stopped = 0;
    std::error_code error;
};
struct sequence_receiver {
    using receiver_concept = ex::receiver_tag;
    sequence_output* output;
    void set_value() && noexcept { ++output->values; }
    void set_error(std::error_code error) && noexcept {
        ++output->errors;
        output->error = error;
    }
    void set_error(std::exception_ptr) && noexcept {
        FAIL_CHECK("Unexpected completion in I/O test");
    }
    void set_stopped() && noexcept { ++output->stopped; }
};

struct query_state {
    unsigned factories = 0, connections = 0, submissions = 0;
    bool factory_throws = false, connect_throws = false, deferred = false, stopped = false;
    std::optional<native_error> error{};
    std::span<const std::byte> command{};
    std::span<std::byte> response{};
    void* pending = nullptr;
    void (*complete_pending)(void*) noexcept = nullptr;
};
struct query_only_endpoint {
    query_state& state;
    struct source {
        using sender_concept = ex::sender_tag;
        query_state* state;
        template <class Env>
        using completion_signatures =
            ex::completion_signatures<ex::set_value_t(), ex::set_error_t(native_error),
                                      ex::set_stopped_t()>;
        template <class Receiver> struct operation {
            using operation_state_concept = ex::operation_state_tag;
            query_state* state;
            Receiver receiver;
            void complete() noexcept {
                if (state->stopped) {
                    ex::set_stopped(std::move(receiver));
                    return;
                }
                if (state->error) {
                    ex::set_error(std::move(receiver), *state->error);
                    return;
                }
                std::fill(state->response.begin(), state->response.end(), std::byte{7});
                ex::set_value(std::move(receiver));
            }
            void start() & noexcept {
                ++state->submissions;
                if (state->deferred) {
                    CHECK(state->pending == nullptr);
                    state->pending = this;
                    state->complete_pending = [](void* pending) noexcept {
                        static_cast<operation*>(pending)->complete();
                    };
                } else {
                    complete();
                }
            }
        };
        auto connect(ex::receiver auto receiver) const {
            ++state->connections;
            if (state->connect_throws)
                throw 43;
            return operation<decltype(receiver)>{state, std::move(receiver)};
        }
    };
    auto async_query(std::span<const std::byte> command, std::span<std::byte> response) {
        ++state.factories;
        if (state.factory_throws)
            throw 42;
        state.command = command;
        state.response = response;
        return source{&state};
    }
};
struct native_query_endpoint : query_only_endpoint {
    auto async_read_some(std::span<std::byte> buffer) noexcept {
        FAIL_CHECK("Native query must take precedence over separate reads");
        return ex::just(std::optional<native_error>{}, buffer.size());
    }
    auto async_write_some(std::span<const std::byte> buffer) noexcept {
        FAIL_CHECK("Native query must take precedence over separate writes");
        return ex::just(std::optional<native_error>{}, buffer.size());
    }
};
struct query_result {
    unsigned values = 0, errors = 0, stopped = 0;
    std::optional<native_error> error;
    int exception = 0;
};
struct query_receiver {
    using receiver_concept = ex::receiver_tag;
    query_result* result;
    void set_value() && noexcept { ++result->values; }
    void set_error(native_error error) && noexcept {
        ++result->errors;
        result->error = error;
    }
    void set_error(std::exception_ptr error) && noexcept {
        ++result->errors;
        try {
            std::rethrow_exception(error);
        } catch (int value) {
            result->exception = value;
        } catch (...) {
            FAIL_CHECK("Unexpected exception in native query test");
        }
    }
    void set_stopped() && noexcept { ++result->stopped; }
};

SCENARIO("Native queries are lazy and take precedence over separate transfers",
         "[io][operations]") {
    query_state state;
    const std::array command{std::byte{1}, std::byte{2}};
    std::array<std::byte, 3> response{};
    query_result output;

    GIVEN("a query-only endpoint with immediate completion") {
        query_only_endpoint endpoint{state};

        WHEN("a query is created directly, connected, and started") {
            auto sender = ex::io::async_query(endpoint, command, response);
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            ex::start(operation);
            CHECK((state.factories == 1 && state.connections == 1 && state.submissions == 1));

            THEN("one native query uses the original buffers without read/write fallback") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.command.data() == command.data() &&
                       state.command.size() == command.size()));
                CHECK((state.response.data() == response.data() &&
                       state.response.size() == response.size()));
                CHECK(std::all_of(response.begin(), response.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }
    }

    GIVEN("an endpoint exposing a native query and separate read/write operations with immediate "
          "completion") {
        native_query_endpoint endpoint{{state}};

        WHEN("a query is created directly, connected, and started") {
            auto sender = ex::io::async_query(endpoint, command, response);
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            ex::start(operation);
            CHECK((state.factories == 1 && state.connections == 1 && state.submissions == 1));

            THEN("one native query uses the original buffers without read/write fallback") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.command.data() == command.data() &&
                       state.command.size() == command.size()));
                CHECK((state.response.data() == response.data() &&
                       state.response.size() == response.size()));
                CHECK(std::all_of(response.begin(), response.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }
    }

    GIVEN("an endpoint exposing a native query and separate read/write operations with immediate "
          "completion") {
        native_query_endpoint endpoint{{state}};

        WHEN("a query follows an explicit command and response sender, connected, and started") {
            auto sender =
                ex::io::async_query(ex::just(std::span{command}, std::span{response}), endpoint);
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            ex::start(operation);
            CHECK((state.factories == 1 && state.connections == 1 && state.submissions == 1));

            THEN("one native query uses the original buffers without read/write fallback") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.command.data() == command.data() &&
                       state.command.size() == command.size()));
                CHECK((state.response.data() == response.data() &&
                       state.response.size() == response.size()));
                CHECK(std::all_of(response.begin(), response.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }
    }

    GIVEN("an endpoint exposing a native query and separate read/write operations with immediate "
          "completion") {
        native_query_endpoint endpoint{{state}};

        WHEN("a query follows command and response buffers through the pipe form, connected, and "
             "started") {
            auto sender =
                ex::just(std::span{command}, std::span{response}) | ex::io::async_query(endpoint);
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            ex::start(operation);
            CHECK((state.factories == 1 && state.connections == 1 && state.submissions == 1));

            THEN("one native query uses the original buffers without read/write fallback") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.command.data() == command.data() &&
                       state.command.size() == command.size()));
                CHECK((state.response.data() == response.data() &&
                       state.response.size() == response.size()));
                CHECK(std::all_of(response.begin(), response.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }
    }

    GIVEN("a query-only endpoint with deferred completion") {
        query_only_endpoint endpoint{state};
        state.deferred = true;

        WHEN("a query is created directly, connected, and started") {
            auto sender = ex::io::async_query(endpoint, command, response);
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            ex::start(operation);
            CHECK((state.factories == 1 && state.connections == 1 && state.submissions == 1));
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("one native query uses the original buffers without read/write fallback") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.command.data() == command.data() &&
                       state.command.size() == command.size()));
                CHECK((state.response.data() == response.data() &&
                       state.response.size() == response.size()));
                CHECK(std::all_of(response.begin(), response.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }
    }

    GIVEN("an endpoint exposing a native query and separate read/write operations with deferred "
          "completion") {
        native_query_endpoint endpoint{{state}};
        state.deferred = true;

        WHEN("a query is created directly, connected, and started") {
            auto sender = ex::io::async_query(endpoint, command, response);
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            ex::start(operation);
            CHECK((state.factories == 1 && state.connections == 1 && state.submissions == 1));
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("one native query uses the original buffers without read/write fallback") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.command.data() == command.data() &&
                       state.command.size() == command.size()));
                CHECK((state.response.data() == response.data() &&
                       state.response.size() == response.size()));
                CHECK(std::all_of(response.begin(), response.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }
    }

    GIVEN("an endpoint exposing a native query and separate read/write operations with deferred "
          "completion") {
        native_query_endpoint endpoint{{state}};
        state.deferred = true;

        WHEN("a query follows an explicit command and response sender, connected, and started") {
            auto sender =
                ex::io::async_query(ex::just(std::span{command}, std::span{response}), endpoint);
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            ex::start(operation);
            CHECK((state.factories == 1 && state.connections == 1 && state.submissions == 1));
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("one native query uses the original buffers without read/write fallback") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.command.data() == command.data() &&
                       state.command.size() == command.size()));
                CHECK((state.response.data() == response.data() &&
                       state.response.size() == response.size()));
                CHECK(std::all_of(response.begin(), response.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }
    }

    GIVEN("an endpoint exposing a native query and separate read/write operations with deferred "
          "completion") {
        native_query_endpoint endpoint{{state}};
        state.deferred = true;

        WHEN("a query follows command and response buffers through the pipe form, connected, and "
             "started") {
            auto sender =
                ex::just(std::span{command}, std::span{response}) | ex::io::async_query(endpoint);
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            CHECK((state.factories == 0 && state.connections == 0 && state.submissions == 0));
            ex::start(operation);
            CHECK((state.factories == 1 && state.connections == 1 && state.submissions == 1));
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("one native query uses the original buffers without read/write fallback") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.command.data() == command.data() &&
                       state.command.size() == command.size()));
                CHECK((state.response.data() == response.data() &&
                       state.response.size() == response.size()));
                CHECK(std::all_of(response.begin(), response.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }
    }
}

SCENARIO("Native query errors and stopped completions do not trigger fallback",
         "[io][operations]") {
    query_state state;
    state.error = native_error::timeout;
    native_query_endpoint endpoint{{state}};
    std::array<std::byte, 3> buffer{};
    query_result output;

    GIVEN("a native query factory that throws") {
        state.factory_throws = true;

        WHEN("the composed sender is started") {
            auto sender = ex::io::async_query(endpoint, buffer, buffer);
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            ex::start(operation);

            THEN("the terminal result is forwarded without submitting a fallback transfer") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.stopped == 0);
                CHECK(output.exception == 42);
                CHECK(state.factories == 1);
                CHECK(state.submissions == 0);
                CHECK(!output.error);
            }
        }
    }

    GIVEN("a native query whose connection throws") {
        state.connect_throws = true;

        WHEN("the composed sender is started") {
            auto sender = ex::io::async_query(endpoint, buffer, buffer);
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            ex::start(operation);

            THEN("the terminal result is forwarded without submitting a fallback transfer") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.stopped == 0);
                CHECK(output.exception == 43);
                CHECK(state.factories == 1);
                CHECK(state.submissions == 0);
                CHECK(!output.error);
            }
        }
    }

    GIVEN("a native query returning an immediate typed error") {
        WHEN("the composed sender is started") {
            auto sender = ex::io::async_query(endpoint, buffer, buffer);
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            ex::start(operation);

            THEN("the terminal result is forwarded without submitting a fallback transfer") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.stopped == 0);
                CHECK(output.exception == 0);
                CHECK(state.factories == 1);
                CHECK(state.submissions == 1);
                CHECK(output.error == native_error::timeout);
            }
        }
    }

    GIVEN("a native query returning a deferred typed error") {
        state.deferred = true;

        WHEN("the composed sender is started") {
            auto sender = ex::io::async_query(endpoint, buffer, buffer);
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            ex::start(operation);
            CHECK((output.errors == 0 && output.values == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("the terminal result is forwarded without submitting a fallback transfer") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.stopped == 0);
                CHECK(output.exception == 0);
                CHECK(state.factories == 1);
                CHECK(state.submissions == 1);
                CHECK(output.error == native_error::timeout);
            }
        }
    }

    GIVEN("a native query returning a stopped completion") {
        state.stopped = true;

        WHEN("the composed sender is started") {
            auto sender = ex::io::async_query(endpoint, buffer, buffer);
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            ex::start(operation);

            THEN("the terminal result is forwarded without submitting a fallback transfer") {
                CHECK(output.values == 0);
                CHECK(output.errors == 0);
                CHECK(output.stopped == 1);
                CHECK(output.exception == 0);
                CHECK(state.factories == 1);
                CHECK(state.submissions == 1);
                CHECK(!output.error);
            }
        }
    }

    GIVEN("a predecessor that completes stopped") {
        WHEN("the composed sender is started") {
            auto sender = ex::just_stopped() | ex::io::async_query(endpoint);
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            ex::start(operation);

            THEN("the terminal result is forwarded without submitting a fallback transfer") {
                CHECK(output.values == 0);
                CHECK(output.errors == 0);
                CHECK(output.stopped == 1);
                CHECK(output.exception == 0);
                CHECK(state.factories == 0);
                CHECK(state.submissions == 0);
                CHECK(!output.error);
            }
        }
    }
}

SCENARIO("Native queries forward a deferred stopped completion", "[io][operations]") {
    query_state state{.deferred = true, .stopped = true};
    query_only_endpoint endpoint{state};
    std::array<std::byte, 3> buffer{};
    query_result output;

    GIVEN("a pending native query that will complete stopped") {
        WHEN("the query starts and its native completion arrives") {
            auto sender = ex::io::async_query(endpoint, buffer, buffer);
            auto operation = ex::connect(std::move(sender), query_receiver{&output});
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("the final receiver gets one stopped completion") {
                CHECK((output.values == 0 && output.errors == 0 && output.stopped == 1));
                CHECK(state.submissions == 1);
            }
        }
    }
}

SCENARIO("Composed queries preserve transfer order and terminal results", "[io][operations]") {
    independent_state state;
    independent_endpoint endpoint{state};
    const std::array command{std::byte{1}, std::byte{2}, std::byte{3}};
    std::array<std::byte, 5> response{};
    state.input.fill(std::byte{7});
    sequence_output output;

    GIVEN("an endpoint acknowledging at most two bytes per primitive") {
        WHEN("a query writes three bytes and reads five bytes") {
            auto sender = ex::io::async_query(endpoint, command, response);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the entire command is written before any response bytes are read") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.write_position == 3 && state.read_position == 5));
                CHECK((state.trace_size == 5 && state.trace[0] == 'w' && state.trace[1] == 'w' &&
                       state.trace[2] == 'r'));
                CHECK(std::equal(command.begin(), command.end(), state.output.begin()));
                CHECK(std::all_of(response.begin(), response.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }
    }

    GIVEN("an endpoint whose partial transfers complete asynchronously") {
        state.deferred = true;

        WHEN("each pending primitive completes in turn") {
            auto sender = ex::io::async_query(endpoint, command, response);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);
            REQUIRE(state.pending != nullptr);
            CHECK((state.read_position == 0 && output.values == 0));
            for (unsigned completions = 0; state.pending; ++completions) {
                REQUIRE(completions < 5);
                REQUIRE(state.write_position <= command.size());
                REQUIRE(state.read_position <= response.size());
                auto* pending = std::exchange(state.pending, nullptr);
                state.complete_pending(pending);
            }

            THEN("the write and read each finish before the composed query completes") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.write_position == 3 && state.read_position == 5));
            }
        }
    }

    GIVEN("the first write returns an I/O condition") {
        state.write_error = true;

        WHEN("the query starts") {
            auto sender = ex::io::async_query(endpoint, command, response);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the query forwards that terminal completion without another transfer") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.stopped == 0);
                CHECK(state.write_position == 0);
                CHECK(state.read_position == 0);
                CHECK(state.trace_size == 1);
            }
        }
    }

    GIVEN("the read phase returns an I/O condition") {
        state.read_error = true;

        WHEN("the query starts") {
            auto sender = ex::io::async_query(endpoint, command, response);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the query forwards that terminal completion without another transfer") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.stopped == 0);
                CHECK(state.write_position == 3);
                CHECK(state.read_position == 0);
                CHECK(state.trace_size == 3);
            }
        }
    }

    GIVEN("the first write completes stopped") {
        state.stop_write = true;

        WHEN("the query starts") {
            auto sender = ex::io::async_query(endpoint, command, response);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the query forwards that terminal completion without another transfer") {
                CHECK(output.values == 0);
                CHECK(output.errors == 0);
                CHECK(output.stopped == 1);
                CHECK(state.write_position == 0);
                CHECK(state.read_position == 0);
                CHECK(state.trace_size == 0);
            }
        }
    }

    GIVEN("a query followed by a separate write") {
        const auto first = std::span{command}.first(1);
        const auto next = std::span{command}.subspan(1, 1);

        WHEN("the query is composed with the following write through let_value") {
            auto sender =
                ex::io::async_query(endpoint, first, std::span{response}.first(1)) |
                ex::let_value([&endpoint, next] { return ex::io::async_write(endpoint, next); });
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the observed transfer order is write, read, then write") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.trace_size == 3 && state.trace[0] == 'w' && state.trace[1] == 'r' &&
                       state.trace[2] == 'w'));
            }
        }
    }

    GIVEN("empty command and response buffers") {
        WHEN("the query starts") {
            auto sender = ex::io::async_query(endpoint, {}, {});
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("it completes successfully without submitting a transfer") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.trace_size == 0 && state.read_position == 0 &&
                       state.write_position == 0));
            }
        }
    }
}

SCENARIO("Exact transfers support directional endpoints", "[io][operations]") {
    independent_state state;
    independent_endpoint endpoint{state};
    const std::array command{std::byte{1}, std::byte{2}, std::byte{3}};
    std::array<std::byte, 5> response{};
    state.input.fill(std::byte{7});
    sequence_output output;

    GIVEN("an endpoint exposing only reads") {
        async_input input{endpoint};

        WHEN("an exact read is followed by another exact read") {
            auto sender = ex::io::async_read(input, response) | ex::let_value([&input, &response] {
                              return ex::io::async_read(input, std::span{response}.first(1));
                          });
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("both reads finish and no write is attempted") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.read_position == 6 && state.write_position == 0));
            }
        }
    }

    GIVEN("an endpoint exposing only writes") {
        async_output writable{endpoint};

        WHEN("an exact write is started") {
            auto sender = ex::io::async_write(writable, command);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the command is acknowledged without attempting a read") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.write_position == 3 && state.read_position == 0));
            }
        }
    }
}

SCENARIO("Exact I/O retains its pending child until native completion", "[io][operations]") {
    independent_state state;
    independent_endpoint endpoint{state};
    std::array<std::byte, 5> buffer{};
    const std::array command{std::byte{1}, std::byte{2}, std::byte{3}};
    sequence_output output;

    GIVEN("a deferred read whose second primitive reports an I/O condition") {
        state.deferred = true;

        WHEN("the first and second native completions arrive") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            state.read_error = true;
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("the receiver stays pending until the terminal native result arrives") {
                CHECK((output.errors == 1 && output.values == 0 && output.stopped == 0));
                CHECK(state.read_position == 2);
                CHECK(state.pending == nullptr);
            }
        }
    }

    GIVEN("a deferred write acknowledged in a two-byte prefix and one final byte") {
        state.deferred = true;

        WHEN("the first and second native completions arrive") {
            auto sender = ex::io::async_write(endpoint, command);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));

            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("the receiver stays pending until the terminal native result arrives") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(state.write_position == 3);
                CHECK(state.pending == nullptr);
            }
        }
    }

    GIVEN("a deferred read whose second primitive completes stopped") {
        state.deferred = true;

        WHEN("the first and second native completions arrive") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            state.stop_read = true;
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("the receiver stays pending until the terminal native result arrives") {
                CHECK((output.stopped == 1 && output.values == 0 && output.errors == 0));
                CHECK(state.read_position == 2);
                CHECK(state.pending == nullptr);
            }
        }
    }
}

SCENARIO("Exact reads handle predecessors and transfer boundaries", "[io][operations]") {
    independent_state state;
    independent_endpoint endpoint{state};
    std::array<std::byte, 5> buffer{};
    sequence_output output;

    GIVEN("a primitive that completes stopped before acknowledging data") {
        state.stop_read = true;

        WHEN("an exact read starts") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("one stopped completion reaches the receiver without reading bytes") {
                CHECK((output.stopped == 1 && output.values == 0 && output.errors == 0));
                CHECK((state.read_position == 0 && state.trace_size == 0));
            }
        }
    }

    GIVEN("a predecessor carrying the destination buffer") {
        WHEN("the predecessor is piped into an exact read") {
            auto sender = ex::just(std::span{buffer}) | ex::io::async_read(endpoint);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the complete buffer is acknowledged once") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(state.read_position == buffer.size());
            }
        }
    }

    GIVEN("an empty destination buffer") {
        WHEN("an exact read starts") {
            auto sender = ex::io::async_read(endpoint, {});
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the receiver completes without submitting a primitive") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK((state.read_position == 0 && state.trace_size == 0));
            }
        }
    }
}

SCENARIO("Exact I/O rejects invalid acknowledged counts", "[io][operations]") {
    struct endpoint {
        std::size_t count = 0;
        unsigned calls = 0;
        auto report() noexcept {
            // A retry ends with an error so a zero-progress regression cannot hang.
            auto error =
                calls++ ? std::optional{std::make_error_code(std::errc::io_error)} : std::nullopt;
            return ex::just(error, count);
        }
        auto async_read_some(std::span<std::byte>) noexcept { return report(); }
        auto async_write_some(std::span<const std::byte>) noexcept { return report(); }
    } object;
    std::array<std::byte, 3> buffer{};
    sequence_output output;

    GIVEN("a read primitive reporting zero bytes without an I/O condition") {
        object.count = 0;

        WHEN("an exact read starts") {
            auto sender = ex::io::async_read(object, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("one io_error is reported without retrying the invalid result") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(object.calls == 1);
            }
        }
    }

    GIVEN("a read primitive reporting more bytes than the supplied buffer") {
        object.count = 4;

        WHEN("an exact read starts") {
            auto sender = ex::io::async_read(object, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("one io_error is reported without retrying the invalid result") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(object.calls == 1);
            }
        }
    }

    GIVEN("a write primitive reporting zero bytes without an I/O condition") {
        object.count = 0;

        WHEN("an exact write starts") {
            auto sender = ex::io::async_write(object, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("one io_error is reported without retrying the invalid result") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(object.calls == 1);
            }
        }
    }

    GIVEN("a write primitive reporting more bytes than the supplied buffer") {
        object.count = 4;

        WHEN("an exact write starts") {
            auto sender = ex::io::async_write(object, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("one io_error is reported without retrying the invalid result") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(object.calls == 1);
            }
        }
    }
}

SCENARIO("Exact I/O derives typed results from primitive completion signatures",
         "[io][operations]") {
    using outcome = choice_endpoint::outcome;
    choice_endpoint endpoint{outcome::success};
    std::array<std::byte, 2> buffer{};
    typed_output output;
    STATIC_REQUIRE(ex::io::readable<choice_endpoint>);
    using exact_read = decltype(ex::io::async_read(endpoint, buffer));
    STATIC_REQUIRE(
        std::same_as<ex::completion_signatures_of_t<exact_read>,
                     ex::completion_signatures<
                         ex::set_error_t(native_error), ex::set_error_t(move_only_error),
                         ex::set_error_t(int), ex::set_value_t(), ex::set_error_t(std::error_code),
                         ex::set_error_t(std::exception_ptr)>>);

    GIVEN("a primitive acknowledging one byte without a condition") {
        endpoint.completion = outcome::success;

        WHEN("an exact read starts") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), typed_receiver{&output});
            ex::start(operation);

            THEN("the facade completes with the corresponding typed result") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(endpoint.calls == 2);
            }
        }
    }

    GIVEN("a primitive returning an enum condition") {
        endpoint.completion = outcome::native_condition;

        WHEN("an exact read starts") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), typed_receiver{&output});
            ex::start(operation);

            THEN("the facade completes with the corresponding typed result") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(endpoint.calls == 1);
            }
        }
    }

    GIVEN("a primitive returning a move-only condition") {
        endpoint.completion = outcome::move_only_condition;

        WHEN("an exact read starts") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), typed_receiver{&output});
            ex::start(operation);

            THEN("the facade completes with the corresponding typed result") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.code == 17);
                CHECK(endpoint.calls == 1);
            }
        }
    }

    GIVEN("a primitive completing through its upstream integer error channel") {
        endpoint.completion = outcome::upstream_error;

        WHEN("an exact read starts") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), typed_receiver{&output});
            ex::start(operation);

            THEN("the facade completes with the corresponding typed result") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.code == 42);
                CHECK(endpoint.calls == 1);
            }
        }
    }
}

SCENARIO("Exact transfers own reference-valued errors", "[io][operations]") {
    const auto condition = std::make_error_code(std::errc::connection_reset);
    borrowed_error_endpoint<std::error_code> endpoint{.error = condition};
    std::array<std::byte, 3> buffer{};
    struct result {
        int values = 0, errors = 0;
        std::error_code error;
    } output;
    struct receiver {
        using receiver_concept = ex::receiver_tag;
        result* output;
        void set_value() && noexcept { ++output->values; }
        void set_error(std::error_code error) && noexcept {
            ++output->errors;
            output->error = error;
        }
        void set_error(std::exception_ptr) && noexcept { FAIL_CHECK("Unexpected exception"); }
        void set_stopped() && noexcept { FAIL_CHECK("Unexpected stopped completion"); }
    };
    using unownable_endpoint = borrowed_error_endpoint<move_only_error>;
    using unownable_read =
        decltype(ex::io::async_read(std::declval<unownable_endpoint&>(), std::span<std::byte>{}));
    STATIC_REQUIRE(!ex::sender_in<unownable_read>);

    GIVEN("a immediate read that acknowledges one byte at a time") {
        endpoint.fail = false;
        endpoint.deferred = false;

        WHEN("the exact transfer finishes and the endpoint clears its error storage") {
            auto sender = ex::io::async_read(endpoint, buffer);
            STATIC_REQUIRE(
                std::same_as<
                    ex::completion_signatures_of_t<decltype(sender)>,
                    ex::completion_signatures<ex::set_error_t(std::error_code), ex::set_value_t(),
                                              ex::set_error_t(std::exception_ptr)>>);
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);
            endpoint.error.clear();

            THEN("the receiver retains an independent terminal result") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(!output.error);
                CHECK(endpoint.calls == 3);
            }
        }
    }

    GIVEN("a deferred read that acknowledges one byte at a time") {
        endpoint.fail = false;
        endpoint.deferred = true;

        WHEN("the exact transfer finishes and the endpoint clears its error storage") {
            auto sender = ex::io::async_read(endpoint, buffer);
            STATIC_REQUIRE(
                std::same_as<
                    ex::completion_signatures_of_t<decltype(sender)>,
                    ex::completion_signatures<ex::set_error_t(std::error_code), ex::set_value_t(),
                                              ex::set_error_t(std::exception_ptr)>>);
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0));
            REQUIRE(endpoint.pending != nullptr);
            for (std::size_t completions = 0; endpoint.pending; ++completions) {
                REQUIRE(completions < buffer.size());
                endpoint.complete_pending(std::exchange(endpoint.pending, nullptr));
            }
            endpoint.error.clear();

            THEN("the receiver retains an independent terminal result") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(!output.error);
                CHECK(endpoint.calls == 3);
            }
        }
    }

    GIVEN("a immediate read that returns a borrowed error") {
        endpoint.fail = true;
        endpoint.deferred = false;

        WHEN("the exact transfer finishes and the endpoint clears its error storage") {
            auto sender = ex::io::async_read(endpoint, buffer);
            STATIC_REQUIRE(
                std::same_as<
                    ex::completion_signatures_of_t<decltype(sender)>,
                    ex::completion_signatures<ex::set_error_t(std::error_code), ex::set_value_t(),
                                              ex::set_error_t(std::exception_ptr)>>);
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);
            endpoint.error.clear();

            THEN("the receiver retains an independent terminal result") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.error == condition);
                CHECK(endpoint.calls == 1);
            }
        }
    }

    GIVEN("a deferred read that returns a borrowed error") {
        endpoint.fail = true;
        endpoint.deferred = true;

        WHEN("the exact transfer finishes and the endpoint clears its error storage") {
            auto sender = ex::io::async_read(endpoint, buffer);
            STATIC_REQUIRE(
                std::same_as<
                    ex::completion_signatures_of_t<decltype(sender)>,
                    ex::completion_signatures<ex::set_error_t(std::error_code), ex::set_value_t(),
                                              ex::set_error_t(std::exception_ptr)>>);
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0));
            REQUIRE(endpoint.pending != nullptr);
            for (std::size_t completions = 0; endpoint.pending; ++completions) {
                REQUIRE(completions < buffer.size());
                endpoint.complete_pending(std::exchange(endpoint.pending, nullptr));
            }
            endpoint.error.clear();

            THEN("the receiver retains an independent terminal result") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.error == condition);
                CHECK(endpoint.calls == 1);
            }
        }
    }

    GIVEN("a immediate write that acknowledges one byte at a time") {
        endpoint.fail = false;
        endpoint.deferred = false;

        WHEN("the exact transfer finishes and the endpoint clears its error storage") {
            auto sender = ex::io::async_write(endpoint, buffer);
            STATIC_REQUIRE(
                std::same_as<
                    ex::completion_signatures_of_t<decltype(sender)>,
                    ex::completion_signatures<ex::set_error_t(std::error_code), ex::set_value_t(),
                                              ex::set_error_t(std::exception_ptr)>>);
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);
            endpoint.error.clear();

            THEN("the receiver retains an independent terminal result") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(!output.error);
                CHECK(endpoint.calls == 3);
            }
        }
    }

    GIVEN("a deferred write that acknowledges one byte at a time") {
        endpoint.fail = false;
        endpoint.deferred = true;

        WHEN("the exact transfer finishes and the endpoint clears its error storage") {
            auto sender = ex::io::async_write(endpoint, buffer);
            STATIC_REQUIRE(
                std::same_as<
                    ex::completion_signatures_of_t<decltype(sender)>,
                    ex::completion_signatures<ex::set_error_t(std::error_code), ex::set_value_t(),
                                              ex::set_error_t(std::exception_ptr)>>);
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0));
            REQUIRE(endpoint.pending != nullptr);
            for (std::size_t completions = 0; endpoint.pending; ++completions) {
                REQUIRE(completions < buffer.size());
                endpoint.complete_pending(std::exchange(endpoint.pending, nullptr));
            }
            endpoint.error.clear();

            THEN("the receiver retains an independent terminal result") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(!output.error);
                CHECK(endpoint.calls == 3);
            }
        }
    }

    GIVEN("a immediate write that returns a borrowed error") {
        endpoint.fail = true;
        endpoint.deferred = false;

        WHEN("the exact transfer finishes and the endpoint clears its error storage") {
            auto sender = ex::io::async_write(endpoint, buffer);
            STATIC_REQUIRE(
                std::same_as<
                    ex::completion_signatures_of_t<decltype(sender)>,
                    ex::completion_signatures<ex::set_error_t(std::error_code), ex::set_value_t(),
                                              ex::set_error_t(std::exception_ptr)>>);
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);
            endpoint.error.clear();

            THEN("the receiver retains an independent terminal result") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.error == condition);
                CHECK(endpoint.calls == 1);
            }
        }
    }

    GIVEN("a deferred write that returns a borrowed error") {
        endpoint.fail = true;
        endpoint.deferred = true;

        WHEN("the exact transfer finishes and the endpoint clears its error storage") {
            auto sender = ex::io::async_write(endpoint, buffer);
            STATIC_REQUIRE(
                std::same_as<
                    ex::completion_signatures_of_t<decltype(sender)>,
                    ex::completion_signatures<ex::set_error_t(std::error_code), ex::set_value_t(),
                                              ex::set_error_t(std::exception_ptr)>>);
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0));
            REQUIRE(endpoint.pending != nullptr);
            for (std::size_t completions = 0; endpoint.pending; ++completions) {
                REQUIRE(completions < buffer.size());
                endpoint.complete_pending(std::exchange(endpoint.pending, nullptr));
            }
            endpoint.error.clear();

            THEN("the receiver retains an independent terminal result") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.error == condition);
                CHECK(endpoint.calls == 1);
            }
        }
    }
}

SCENARIO("Exact transfers handle exceptions while owning borrowed errors", "[io][exceptions]") {
    struct result {
        unsigned errors = 0, exceptions = 0;
        int code = 0;
    } output;
    struct error_receiver {
        using receiver_concept = ex::receiver_tag;
        result* output;
        void set_value() && noexcept { FAIL_CHECK("Unexpected value"); }
        void set_error(copied_error error) && noexcept {
            ++output->errors;
            output->code = error.code;
        }
        void set_error(std::error_code) && noexcept { FAIL_CHECK("Unexpected progress error"); }
        void set_error(std::exception_ptr error) && noexcept {
            ++output->exceptions;
            try {
                std::rethrow_exception(error);
            } catch (int value) {
                output->code = value;
            } catch (...) {
                FAIL_CHECK("Unexpected exception");
            }
        }
        void set_stopped() && noexcept { FAIL_CHECK("Unexpected stopped completion"); }
    };
    std::array<std::byte, 3> buffer{};

    GIVEN("a copyable borrowed error") {
        borrowed_error_endpoint<copied_error> endpoint{.error = copied_error{17, false},
                                                       .fail = true};

        WHEN("an exact read receives the borrowed error") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), error_receiver{&output});
            ex::start(operation);

            THEN("the receiver owns the copied typed error") {
                CHECK(output.errors == 1);
                CHECK(output.exceptions == 0);
                CHECK(output.code == 17);
                CHECK(endpoint.calls == 1);
            }
        }
    }

    GIVEN("a borrowed error that throws when copied") {
        borrowed_error_endpoint<copied_error> endpoint{.error = copied_error{17, true},
                                                       .fail = true};

        WHEN("an exact read receives the borrowed error") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), error_receiver{&output});
            ex::start(operation);

            THEN("the copy exception reaches the exception channel") {
                CHECK(output.errors == 0);
                CHECK(output.exceptions == 1);
                CHECK(output.code == 47);
                CHECK(endpoint.calls == 1);
            }
        }
    }
}

SCENARIO("Exact I/O rejects conditions regardless of acknowledged count", "[io][operations]") {
    independent_state state;
    independent_endpoint endpoint{state};
    std::array<std::byte, 3> buffer{};
    sequence_output output;

    GIVEN("a immediate read reporting a condition after acknowledging no bytes") {
        state.read_error = true;
        state.error_count = 0;
        state.deferred = false;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.read_position == 0);
            }
        }
    }

    GIVEN("a immediate read reporting a condition after acknowledging a two-byte prefix") {
        state.read_error = true;
        state.error_count = 2;
        state.deferred = false;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.read_position == 2);
            }
        }
    }

    GIVEN(
        "a immediate read reporting a condition after acknowledging the entire three-byte buffer") {
        state.read_error = true;
        state.error_count = 3;
        state.deferred = false;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.read_position == 3);
            }
        }
    }

    GIVEN("a deferred read reporting a condition after acknowledging no bytes") {
        state.read_error = true;
        state.error_count = 0;
        state.deferred = true;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.read_position == 0);
            }
        }
    }

    GIVEN("a deferred read reporting a condition after acknowledging a two-byte prefix") {
        state.read_error = true;
        state.error_count = 2;
        state.deferred = true;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.read_position == 2);
            }
        }
    }

    GIVEN(
        "a deferred read reporting a condition after acknowledging the entire three-byte buffer") {
        state.read_error = true;
        state.error_count = 3;
        state.deferred = true;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.read_position == 3);
            }
        }
    }

    GIVEN("a immediate write reporting a condition after acknowledging no bytes") {
        state.write_error = true;
        state.error_count = 0;
        state.deferred = false;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_write(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.write_position == 0);
            }
        }
    }

    GIVEN("a immediate write reporting a condition after acknowledging a two-byte prefix") {
        state.write_error = true;
        state.error_count = 2;
        state.deferred = false;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_write(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.write_position == 2);
            }
        }
    }

    GIVEN("a immediate write reporting a condition after acknowledging the entire three-byte "
          "buffer") {
        state.write_error = true;
        state.error_count = 3;
        state.deferred = false;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_write(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.write_position == 3);
            }
        }
    }

    GIVEN("a deferred write reporting a condition after acknowledging no bytes") {
        state.write_error = true;
        state.error_count = 0;
        state.deferred = true;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_write(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.write_position == 0);
            }
        }
    }

    GIVEN("a deferred write reporting a condition after acknowledging a two-byte prefix") {
        state.write_error = true;
        state.error_count = 2;
        state.deferred = true;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_write(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.write_position == 2);
            }
        }
    }

    GIVEN(
        "a deferred write reporting a condition after acknowledging the entire three-byte buffer") {
        state.write_error = true;
        state.error_count = 3;
        state.deferred = true;

        WHEN("the exact transfer completes") {
            auto sender = ex::io::async_write(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), sequence_receiver{&output});
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(state.pending != nullptr);
            state.complete_pending(std::exchange(state.pending, nullptr));

            THEN("the condition is an error even if the acknowledgement covers the whole buffer") {
                CHECK((output.values == 0 && output.errors == 1 && output.stopped == 0));
                CHECK(output.error == std::errc::io_error);
                CHECK(state.trace_size == 1);
                CHECK(state.write_position == 3);
            }
        }
    }
}

} // namespace
