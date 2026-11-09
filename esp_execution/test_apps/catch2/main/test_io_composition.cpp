// SPDX-License-Identifier: Apache-2.0
#include "esp_execution.hpp"
#include "esp_execution/io/operations.hpp"
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <system_error>
#include <array>
#include <algorithm>
namespace {
namespace ex = esp::execution;
struct endpoint {
    int calls = 0;
    auto async_read_some(std::span<std::byte> b) {
        return ex::just() | ex::let_value([this, b]() noexcept {
                   ++calls;
                   b[0] = std::byte{7};
                   return ex::just(std::optional<std::error_code>{}, std::size_t{1});
               });
    }
};
struct receiver {
    using receiver_concept = ex::receiver_tag;
    int* values;
    auto get_env() const noexcept { return ex::empty_env{}; }
    void set_value(auto...) && noexcept { ++*values; }
    void set_error(auto) && noexcept { FAIL_CHECK("Unexpected completion in I/O test"); }
    void set_stopped() && noexcept { FAIL_CHECK("Unexpected completion in I/O test"); }
};

struct failure_state {
    int factory_calls = 0, connections = 0;
    int fail_factory_at = -1;
    bool fail_connect = false;
};
struct fallible_sender {
    using sender_concept = ex::sender_tag;
    failure_state* state;
    template <class Env>
    using completion_signatures =
        ex::completion_signatures<ex::set_value_t(std::optional<std::error_code>, std::size_t)>;
    auto get_env() const noexcept { return ex::empty_env{}; }
    auto connect(ex::receiver auto receiver) const {
        ++state->connections;
        if (state->fail_connect)
            throw 43;
        return ex::connect(ex::just(std::optional<std::error_code>{}, std::size_t{1}),
                           std::move(receiver));
    }
};
struct fallible_endpoint {
    failure_state* state;
    auto make_sender() {
        if (state->factory_calls++ == state->fail_factory_at)
            throw 42;
        return fallible_sender{state};
    }
    auto async_read_some(std::span<std::byte>) { return make_sender(); }
    auto async_write_some(std::span<const std::byte>) { return make_sender(); }
};
struct failure_output {
    int values = 0, exceptions = 0, errors = 0, exception_value = 0;
};
struct failure_receiver {
    using receiver_concept = ex::receiver_tag;
    failure_output* output;
    auto get_env() const noexcept { return ex::empty_env{}; }
    void set_value(auto...) && noexcept { ++output->values; }
    void set_error(std::error_code) && noexcept { ++output->errors; }
    void set_error(std::exception_ptr error) && noexcept {
        ++output->exceptions;
        try {
            std::rethrow_exception(error);
        } catch (int value) {
            output->exception_value = value;
        } catch (...) {
            FAIL_CHECK("Unexpected completion in I/O test");
        }
    }
    void set_stopped() && noexcept { FAIL_CHECK("Unexpected completion in I/O test"); }
};

template <class Source> struct copy_throwing_sender {
    using sender_concept = ex::sender_tag;
    Source source;
    unsigned* copies;

    copy_throwing_sender(Source source, unsigned& copies) noexcept
        : source(std::move(source)), copies(&copies) {}
    copy_throwing_sender(const copy_throwing_sender& other)
        : source(other.source), copies(other.copies) {
        ++*copies;
        throw 44;
    }
    copy_throwing_sender(copy_throwing_sender&&) noexcept = default;

    template <class Env> using completion_signatures = ex::completion_signatures_of_t<Source, Env>;
    decltype(auto) get_env() const noexcept { return ex::get_env(source); }
    auto connect(ex::receiver auto receiver) const
        noexcept(noexcept(ex::connect(source, std::move(receiver)))) {
        return ex::connect(source, std::move(receiver));
    }
};

struct reference_endpoint {
    unsigned copies = 0;
    using transfer_source = decltype(ex::just(std::optional<std::error_code>{}, std::size_t{1}));
    using query_source = decltype(ex::just());
    copy_throwing_sender<transfer_source> transfer{
        ex::just(std::optional<std::error_code>{}, std::size_t{1}), copies};
    copy_throwing_sender<query_source> query{ex::just(), copies};

    auto& async_read_some(std::span<std::byte>) noexcept { return transfer; }
    auto& async_write_some(std::span<const std::byte>) noexcept { return transfer; }
    auto& async_query(std::span<const std::byte>, std::span<std::byte>) noexcept { return query; }
};

SCENARIO("Deferred I/O factories report exceptions while copying sender references",
         "[io][exceptions]") {
    reference_endpoint endpoint;
    std::array<std::byte, 1> buffer{};
    failure_output output;

    GIVEN("a noexcept read factory returning a sender reference whose copy throws") {
        WHEN("a primitive read is connected and started") {
            auto sender = ex::io::async_read_some(endpoint, buffer);
            CHECK(endpoint.copies == 0);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            CHECK(endpoint.copies == 0);
            ex::start(operation);

            THEN("the copy exception reaches the receiver instead of terminating") {
                CHECK(endpoint.copies == 1);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 44);
                CHECK((output.values == 0 && output.errors == 0));
            }
        }

        WHEN("an exact read is connected and started") {
            auto sender = ex::io::async_read(endpoint, buffer);
            CHECK(endpoint.copies == 0);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            CHECK(endpoint.copies == 0);
            ex::start(operation);

            THEN("the facade forwards the copy exception from its primitive") {
                CHECK(endpoint.copies == 1);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 44);
                CHECK((output.values == 0 && output.errors == 0));
            }
        }
    }

    GIVEN("a noexcept write factory returning a sender reference whose copy throws") {
        WHEN("a primitive write is connected and started") {
            auto sender = ex::io::async_write_some(endpoint, buffer);
            CHECK(endpoint.copies == 0);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            CHECK(endpoint.copies == 0);
            ex::start(operation);

            THEN("the copy exception reaches the receiver instead of terminating") {
                CHECK(endpoint.copies == 1);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 44);
                CHECK((output.values == 0 && output.errors == 0));
            }
        }

        WHEN("an exact write is connected and started") {
            auto sender = ex::io::async_write(endpoint, buffer);
            CHECK(endpoint.copies == 0);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            CHECK(endpoint.copies == 0);
            ex::start(operation);

            THEN("the facade forwards the copy exception from its primitive") {
                CHECK(endpoint.copies == 1);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 44);
                CHECK((output.values == 0 && output.errors == 0));
            }
        }
    }

    GIVEN("a noexcept native query factory returning a sender reference whose copy throws") {
        WHEN("a native query is connected and started") {
            auto sender = ex::io::async_query(endpoint, buffer, buffer);
            CHECK(endpoint.copies == 0);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            CHECK(endpoint.copies == 0);
            ex::start(operation);

            THEN("the copy exception reaches the receiver instead of terminating") {
                CHECK(endpoint.copies == 1);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 44);
                CHECK((output.values == 0 && output.errors == 0));
            }
        }
    }
}

SCENARIO("I/O compositions propagate factory and connection exceptions", "[io][exceptions]") {
    std::array<std::byte, 2> bytes{};
    std::span<std::byte> buffer{bytes};
    failure_state state;
    fallible_endpoint endpoint{&state};
    failure_output output;
    using exact_read = decltype(ex::io::async_read(endpoint, buffer));
    STATIC_REQUIRE(
        std::same_as<
            ex::completion_signatures_of_t<exact_read>,
            ex::completion_signatures<ex::set_error_t(std::error_code),
                                      ex::set_error_t(std::exception_ptr), ex::set_value_t()>>);

    GIVEN("an endpoint whose primitive sender factory throws") {
        state.fail_factory_at = 0;

        WHEN("an exact read is started") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            ex::start(operation);

            THEN("the original exception reaches the receiver") {
                CHECK(output.values == 0);
                CHECK(output.errors == 0);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 42);
                CHECK(state.factory_calls == 1);
                CHECK(state.connections == 0);
            }
        }

        WHEN("an exact write follows a buffer sender") {
            auto sender = ex::just(buffer) | ex::io::async_write(endpoint);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            ex::start(operation);

            THEN("the original exception reaches the receiver") {
                CHECK(output.values == 0);
                CHECK(output.errors == 0);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 42);
                CHECK(state.factory_calls == 1);
                CHECK(state.connections == 0);
            }
        }

        WHEN("a composed query follows command and response buffers") {
            auto sender = ex::just(buffer, buffer) | ex::io::async_query(endpoint);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            ex::start(operation);

            THEN("the original exception reaches the receiver") {
                CHECK(output.values == 0);
                CHECK(output.errors == 0);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 42);
                CHECK(state.factory_calls == 1);
                CHECK(state.connections == 0);
            }
        }
    }

    GIVEN("an endpoint whose primitive connection throws") {
        state.fail_connect = true;

        WHEN("an exact read is started") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            ex::start(operation);

            THEN("the original exception reaches the receiver") {
                CHECK(output.values == 0);
                CHECK(output.errors == 0);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 43);
                CHECK(state.factory_calls == 1);
                CHECK(state.connections == 1);
            }
        }

        WHEN("an exact write follows a buffer sender") {
            auto sender = ex::just(buffer) | ex::io::async_write(endpoint);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            ex::start(operation);

            THEN("the original exception reaches the receiver") {
                CHECK(output.values == 0);
                CHECK(output.errors == 0);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 43);
                CHECK(state.factory_calls == 1);
                CHECK(state.connections == 1);
            }
        }

        WHEN("a composed query follows command and response buffers") {
            auto sender = ex::just(buffer, buffer) | ex::io::async_query(endpoint);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            ex::start(operation);

            THEN("the original exception reaches the receiver") {
                CHECK(output.values == 0);
                CHECK(output.errors == 0);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 43);
                CHECK(state.factory_calls == 1);
                CHECK(state.connections == 1);
            }
        }
    }

    GIVEN("a second read primitive cannot be constructed") {
        state.fail_factory_at = 1;

        WHEN("the composed operation is started") {
            auto sender = ex::io::async_read(endpoint, buffer);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            ex::start(operation);

            THEN("completed primitives are followed by one exception completion") {
                CHECK(output.values == 0);
                CHECK(output.errors == 0);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 42);
                CHECK(state.factory_calls == 2);
                CHECK(state.connections == 1);
            }
        }
    }

    GIVEN("the read phase of a composed query cannot be constructed") {
        state.fail_factory_at = 2;

        WHEN("the composed operation is started") {
            auto sender = ex::io::async_query(endpoint, buffer, buffer);
            auto operation = ex::connect(std::move(sender), failure_receiver{&output});
            ex::start(operation);

            THEN("completed primitives are followed by one exception completion") {
                CHECK(output.values == 0);
                CHECK(output.errors == 0);
                CHECK(output.exceptions == 1);
                CHECK(output.exception_value == 42);
                CHECK(state.factory_calls == 3);
                CHECK(state.connections == 2);
            }
        }
    }
}

} // namespace

SCENARIO("I/O operations compose with predecessors", "[io][composition]") {
    endpoint ep;
    static std::array<std::byte, 10000> buffer;
    buffer.fill(std::byte{});
    int values = 0;

    GIVEN("an endpoint acknowledging one byte per primitive") {
        WHEN("a primitive read is started directly") {
            auto sender = ex::io::async_read_some(ep, buffer);
            CHECK(ep.calls == 0);
            auto operation = ex::connect(sender, receiver{&values});
            ex::start(operation);

            THEN("one primitive read acknowledges the first byte") {
                CHECK(values == 1);
                CHECK(ep.calls == 1);
                CHECK(buffer.front() == std::byte{7});
            }
        }

        WHEN("a primitive read follows a buffer through the pipe form") {
            auto sender = ex::just(std::span{buffer}) | ex::io::async_read_some(ep);
            CHECK(ep.calls == 0);
            auto operation = ex::connect(sender, receiver{&values});
            ex::start(operation);

            THEN("one primitive read acknowledges the first byte") {
                CHECK(values == 1);
                CHECK(ep.calls == 1);
                CHECK(buffer.front() == std::byte{7});
            }
        }

        WHEN("a primitive read follows an explicit buffer sender") {
            auto sender = ex::io::async_read_some(ex::just(std::span{buffer}), ep);
            CHECK(ep.calls == 0);
            auto operation = ex::connect(sender, receiver{&values});
            ex::start(operation);

            THEN("one primitive read acknowledges the first byte") {
                CHECK(values == 1);
                CHECK(ep.calls == 1);
                CHECK(buffer.front() == std::byte{7});
            }
        }

        WHEN("an exact read requests all ten thousand bytes") {
            auto sender = ex::io::async_read(ep, buffer);
            auto operation = ex::connect(sender, receiver{&values});
            ex::start(operation);

            THEN("the synchronous primitive loop completes the whole buffer once") {
                CHECK(values == 1);
                CHECK(ep.calls == buffer.size());
                CHECK(std::all_of(buffer.begin(), buffer.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }
    }
}
