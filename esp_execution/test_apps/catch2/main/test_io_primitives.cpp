// SPDX-License-Identifier: Apache-2.0
#include "esp_execution/io/primitives.hpp"
#include "esp_execution/just.hpp"
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <system_error>
#include <algorithm>
#include <array>
#include <exception>
#include <utility>

namespace {
namespace ex = esp::execution;
struct result {
    unsigned values = 0, errors = 0, stopped = 0;
    std::size_t count = 0;
    std::error_code error;
    int exception = 0;
};
struct receiver {
    using receiver_concept = ex::receiver_tag;
    result* output;
    auto get_env() const noexcept { return ex::empty_env{}; }
    void set_value(std::optional<std::error_code> error, std::size_t count) && noexcept {
        output->error = error.value_or(std::error_code{});
        ++output->values;
        output->count = count;
    }
    void set_error(std::error_code error) && noexcept {
        ++output->errors;
        output->error = error;
    }
    void set_error(std::exception_ptr error) && noexcept {
        ++output->errors;
        try {
            std::rethrow_exception(error);
        } catch (int value) {
            output->exception = value;
        } catch (...) {
            FAIL_CHECK("Unexpected completion in I/O test");
        }
    }
    void set_stopped() && noexcept { ++output->stopped; }
};
struct endpoint {
    unsigned factories = 0, connections = 0, reads = 0, writes = 0;
    bool factory_throws = false, connect_throws = false, io_error = false;
    bool stopped = false, deferred = false;
    std::size_t error_count = 0;
    std::array<std::byte, 8> output{};
    void* pending = nullptr;
    void (*complete_pending)(void*) noexcept = nullptr;

    auto read_some(std::span<std::byte> buffer) noexcept
        -> std::pair<std::optional<std::error_code>, std::size_t> {
        ++reads;
        const auto count = std::min(io_error ? error_count : std::size_t{2}, buffer.size());
        std::fill_n(buffer.begin(), count, std::byte{7});
        return {io_error ? std::optional{std::make_error_code(std::errc::io_error)} : std::nullopt,
                count};
    }
    auto write_some(std::span<const std::byte> buffer) noexcept
        -> std::pair<std::optional<std::error_code>, std::size_t> {
        ++writes;
        const auto count = std::min(io_error ? error_count : std::size_t{2}, buffer.size());
        std::copy_n(buffer.begin(), count, output.begin());
        return {io_error ? std::optional{std::make_error_code(std::errc::io_error)} : std::nullopt,
                count};
    }
    struct source {
        using sender_concept = ex::sender_tag;
        endpoint* owner;
        bool write;
        std::span<std::byte> destination;
        std::span<const std::byte> command;
        template <class Env>
        using completion_signatures =
            ex::completion_signatures<ex::set_value_t(std::optional<std::error_code>, std::size_t),
                                      ex::set_stopped_t()>;
        auto get_env() const noexcept { return ex::empty_env{}; }
        template <class Receiver> struct operation {
            using operation_state_concept = ex::operation_state_tag;
            endpoint* owner;
            bool write;
            std::span<std::byte> destination;
            std::span<const std::byte> command;
            Receiver receiver;
            void complete() noexcept {
                if (owner->stopped) {
                    ex::set_stopped(std::move(receiver));
                    return;
                }
                auto [error, count] =
                    write ? owner->write_some(command) : owner->read_some(destination);
                ex::set_value(std::move(receiver), error, count);
            }
            void start() & noexcept {
                if (owner->deferred) {
                    CHECK(!owner->pending);
                    owner->pending = this;
                    owner->complete_pending = [](void* pending) noexcept {
                        static_cast<operation*>(pending)->complete();
                    };
                } else
                    complete();
            }
        };
        auto connect(ex::receiver auto receiver) const {
            ++owner->connections;
            if (owner->connect_throws)
                throw 43;
            return operation<decltype(receiver)>{owner, write, destination, command,
                                                 std::move(receiver)};
        }
    };
    auto async_read_some(std::span<std::byte> buffer) {
        ++factories;
        if (factory_throws)
            throw 42;
        return source{this, false, buffer, {}};
    }
    auto async_write_some(std::span<const std::byte> buffer) {
        ++factories;
        if (factory_throws)
            throw 42;
        return source{this, true, {}, buffer};
    }
};
TEST_CASE("I/O primitives are lazy in all call forms", "[io][primitives]") {
    endpoint ep;
    std::array<std::byte, 3> buffer;
    const std::array command{std::byte{1}, std::byte{2}, std::byte{3}};
    auto read = [&ep, &buffer](auto make_source) {
        ep = {};
        buffer.fill(std::byte{0xff});
        auto source = make_source();
        CHECK((ep.factories == 0 && ep.connections == 0 && ep.reads == 0));
        result output;
        auto operation = ex::connect(std::move(source), receiver{&output});
        CHECK((output.values == 0 && ep.factories == 0 && ep.connections == 0 && ep.reads == 0));
        ex::start(operation);
        CHECK(
            (output.values == 1 && output.errors == 0 && output.stopped == 0 && output.count == 2));
        CHECK((ep.factories == 1 && ep.connections == 1 && ep.reads == 1));
        CHECK((buffer[0] == std::byte{7} && buffer[1] == std::byte{7} &&
               buffer[2] == std::byte{0xff}));
    };
    read([&ep, &buffer] { return ex::io::async_read_some(ep, buffer); });
    read([&ep, &buffer] { return ex::io::async_read_some(ex::just(std::span{buffer}), ep); });
    read([&ep, &buffer] { return ex::just(std::span{buffer}) | ex::io::async_read_some(ep); });
    auto write = [&ep, &command](auto make_source) {
        ep = {};
        auto source = make_source();
        CHECK((ep.factories == 0 && ep.connections == 0 && ep.writes == 0));
        result output;
        auto operation = ex::connect(std::move(source), receiver{&output});
        CHECK((output.values == 0 && ep.factories == 0 && ep.connections == 0 && ep.writes == 0));
        ex::start(operation);
        CHECK(
            (output.values == 1 && output.errors == 0 && output.stopped == 0 && output.count == 2));
        CHECK((ep.factories == 1 && ep.connections == 1 && ep.writes == 1));
        CHECK((ep.output[0] == command[0] && ep.output[1] == command[1] &&
               ep.output[2] == std::byte{}));
    };
    write([&ep, &command] { return ex::io::async_write_some(ep, command); });
    write([&ep, &command] { return ex::io::async_write_some(ex::just(std::span{command}), ep); });
    write([&ep, &command] { return ex::just(std::span{command}) | ex::io::async_write_some(ep); });
}
TEST_CASE("I/O primitives forward errors and cancellation", "[io][primitives]") {
    endpoint ep;
    std::array<std::byte, 3> buffer{};
    auto finish = [](ex::sender auto source) {
        result output;
        auto operation = ex::connect(std::move(source), receiver{&output});
        ex::start(operation);
        return output;
    };
    for (bool connect_throws : {false, true}) {
        ep = {};
        ep.factory_throws = !connect_throws;
        ep.connect_throws = connect_throws;
        const auto read = finish(ex::just(std::span{buffer}) | ex::io::async_read_some(ep));
        CHECK(
            (read.errors == 1 && read.exception == (connect_throws ? 43 : 42) && read.values == 0));
        const auto write = finish(ex::io::async_write_some(ex::just(std::span{buffer}), ep));
        CHECK((write.errors == 1 && write.exception == (connect_throws ? 43 : 42) &&
               write.values == 0));
        CHECK((ep.reads == 0 && ep.writes == 0));
    }
    ep = {};
    ep.factory_throws = true;
    const auto direct_read = finish(ex::io::async_read_some(ep, buffer));
    const auto direct_write = finish(ex::io::async_write_some(ep, buffer));
    CHECK((direct_read.errors == 1 && direct_read.exception == 42));
    CHECK((direct_write.errors == 1 && direct_write.exception == 42));
    ep = {};
    ep.io_error = true;
    const auto error = std::make_error_code(std::errc::io_error);
    const auto read = finish(ex::io::async_read_some(ep, buffer));
    const auto write = finish(ex::just(std::span{buffer}) | ex::io::async_write_some(ep));
    CHECK((read.values == 1 && read.errors == 0 && read.error == error && read.count == 0));
    CHECK((write.values == 1 && write.errors == 0 && write.error == error && write.count == 0));
    ep = {};
    const auto upstream_error = finish(ex::just_error(error) | ex::io::async_read_some(ep));
    const auto upstream_stopped = finish(ex::just_stopped() | ex::io::async_write_some(ep));
    CHECK((upstream_error.errors == 1 && upstream_error.error == error));
    CHECK((upstream_stopped.stopped == 1 && ep.factories == 0));
    ep.stopped = true;
    CHECK(finish(ex::io::async_read_some(ep, buffer)).stopped == 1);
    CHECK((ep.factories == 1 && ep.reads == 0));
}
TEST_CASE("I/O primitives retain pending operation state", "[io][primitives]") {
    endpoint ep;
    ep.deferred = true;
    std::array<std::byte, 3> buffer{};
    result output;
    auto source = ex::just(std::span{buffer}) | ex::io::async_read_some(ep);
    using signatures = ex::completion_signatures_of_t<decltype(source)>;
    STATIC_REQUIRE(
        std::same_as<signatures, ex::completion_signatures<
                                     ex::set_value_t(std::optional<std::error_code>, std::size_t),
                                     ex::set_stopped_t(), ex::set_error_t(std::exception_ptr)>>);
    auto operation = ex::connect(std::move(source), receiver{&output});
    CHECK(ep.factories == 0);
    ex::start(operation);
    REQUIRE(ep.pending != nullptr);
    CHECK((output.values == 0 && ep.reads == 0));
    auto* pending = std::exchange(ep.pending, nullptr);
    ep.complete_pending(pending);
    CHECK((output.values == 1 && output.count == 2 && ep.reads == 1));
}
SCENARIO("I/O conditions preserve acknowledged bytes", "[io][primitives]") {
    endpoint ep;
    ep.io_error = true;
    std::array buffer{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
    const std::array command{std::byte{1}, std::byte{2}, std::byte{3}};
    const auto condition = std::make_error_code(std::errc::io_error);
    result output;

    GIVEN("an immediately completing read that reports an I/O condition") {
        auto operation = ex::connect(ex::io::async_read_some(ep, buffer), receiver{&output});

        WHEN("the endpoint acknowledges no bytes") {
            ep.error_count = 0;
            ex::start(operation);

            THEN("the condition and zero count are delivered with the destination unchanged") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 0);
                CHECK((buffer == std::array{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}}));
            }
        }

        WHEN("the endpoint acknowledges only the first two bytes") {
            ep.error_count = 2;
            ex::start(operation);

            THEN("the condition and count preserve the transferred prefix") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 2);
                CHECK((buffer == std::array{std::byte{7}, std::byte{7}, std::byte{0xff}}));
            }
        }

        WHEN("the endpoint acknowledges all three bytes") {
            ep.error_count = 3;
            ex::start(operation);

            THEN("the condition and count preserve the complete transfer") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 3);
                CHECK((buffer == std::array{std::byte{7}, std::byte{7}, std::byte{7}}));
            }
        }
    }

    GIVEN("a deferred read that reports an I/O condition") {
        ep.deferred = true;
        auto operation = ex::connect(ex::io::async_read_some(ep, buffer), receiver{&output});

        WHEN("the endpoint acknowledges no bytes") {
            ep.error_count = 0;
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(ep.pending != nullptr);
            ep.complete_pending(std::exchange(ep.pending, nullptr));

            THEN("the condition and zero count are delivered with the destination unchanged") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 0);
                CHECK((buffer == std::array{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}}));
            }
        }

        WHEN("the endpoint acknowledges only the first two bytes") {
            ep.error_count = 2;
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(ep.pending != nullptr);
            ep.complete_pending(std::exchange(ep.pending, nullptr));

            THEN("the condition and count preserve the transferred prefix") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 2);
                CHECK((buffer == std::array{std::byte{7}, std::byte{7}, std::byte{0xff}}));
            }
        }

        WHEN("the endpoint acknowledges all three bytes") {
            ep.error_count = 3;
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(ep.pending != nullptr);
            ep.complete_pending(std::exchange(ep.pending, nullptr));

            THEN("the condition and count preserve the complete transfer") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 3);
                CHECK((buffer == std::array{std::byte{7}, std::byte{7}, std::byte{7}}));
            }
        }
    }

    GIVEN("an immediately completing write that reports an I/O condition") {
        auto operation = ex::connect(ex::io::async_write_some(ep, command), receiver{&output});

        WHEN("the endpoint acknowledges no bytes") {
            ep.error_count = 0;
            ex::start(operation);

            THEN("the condition and zero count are delivered with the written data unchanged") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 0);
                CHECK((ep.output == std::array<std::byte, 8>{}));
            }
        }

        WHEN("the endpoint acknowledges only the first two bytes") {
            ep.error_count = 2;
            ex::start(operation);

            THEN("the condition and count preserve the transferred prefix") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 2);
                CHECK((ep.output == std::array<std::byte, 8>{std::byte{1}, std::byte{2}}));
            }
        }

        WHEN("the endpoint acknowledges all three bytes") {
            ep.error_count = 3;
            ex::start(operation);

            THEN("the condition and count preserve the complete transfer") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 3);
                CHECK((ep.output ==
                       std::array<std::byte, 8>{std::byte{1}, std::byte{2}, std::byte{3}}));
            }
        }
    }

    GIVEN("a deferred write that reports an I/O condition") {
        ep.deferred = true;
        auto operation = ex::connect(ex::io::async_write_some(ep, command), receiver{&output});

        WHEN("the endpoint acknowledges no bytes") {
            ep.error_count = 0;
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(ep.pending != nullptr);
            ep.complete_pending(std::exchange(ep.pending, nullptr));

            THEN("the condition and zero count are delivered with the written data unchanged") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 0);
                CHECK((ep.output == std::array<std::byte, 8>{}));
            }
        }

        WHEN("the endpoint acknowledges only the first two bytes") {
            ep.error_count = 2;
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(ep.pending != nullptr);
            ep.complete_pending(std::exchange(ep.pending, nullptr));

            THEN("the condition and count preserve the transferred prefix") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 2);
                CHECK((ep.output == std::array<std::byte, 8>{std::byte{1}, std::byte{2}}));
            }
        }

        WHEN("the endpoint acknowledges all three bytes") {
            ep.error_count = 3;
            ex::start(operation);
            CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            REQUIRE(ep.pending != nullptr);
            ep.complete_pending(std::exchange(ep.pending, nullptr));

            THEN("the condition and count preserve the complete transfer") {
                CHECK((output.values == 1 && output.errors == 0 && output.stopped == 0));
                CHECK(output.error == condition);
                CHECK(output.count == 3);
                CHECK((ep.output ==
                       std::array<std::byte, 8>{std::byte{1}, std::byte{2}, std::byte{3}}));
            }
        }
    }
}

} // namespace
