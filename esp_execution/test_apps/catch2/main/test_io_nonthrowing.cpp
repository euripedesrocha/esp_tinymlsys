// SPDX-License-Identifier: Apache-2.0
#include "esp_execution/io/operations.hpp"
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <array>

namespace {
namespace ex = esp::execution;
enum class native_error { failed, timeout };
struct device {
    std::optional<native_error> condition;
    auto async_read_some(std::span<std::byte> buffer) noexcept {
        return ex::just(condition, buffer.size());
    }
    auto async_write_some(std::span<const std::byte> buffer) noexcept {
        return ex::just(condition, buffer.size());
    }
};
struct receiver {
    using receiver_concept = ex::receiver_tag;
    std::optional<native_error>* condition;
    std::size_t* count;
    void set_value(std::optional<native_error> error, std::size_t n) && noexcept {
        *condition = error;
        *count = n;
    }
};

} // namespace

SCENARIO("I/O primitives execute with exceptions disabled", "[io][no_exceptions]") {
    device object{};
    std::array<std::byte, 4> buffer{};
    std::optional<native_error> condition;
    std::size_t count = 0;

    GIVEN("a nonthrowing successful endpoint") {
        WHEN("a read primitive is started") {
            auto sender = ex::io::async_read_some(object, buffer);
            STATIC_REQUIRE(std::same_as<ex::completion_signatures_of_t<decltype(sender)>,
                                        ex::completion_signatures<ex::set_value_t(
                                            std::optional<native_error>, std::size_t)>>);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), receiver{})));
            auto operation = ex::connect(std::move(sender), receiver{&condition, &count});
            ex::start(operation);

            THEN("the condition and acknowledged count arrive through value completion") {
                CHECK(count == 4);
                CHECK(!condition);
            }
        }

        WHEN("a write primitive follows a buffer sender") {
            auto sender = ex::just(std::span{buffer}) | ex::io::async_write_some(object);
            STATIC_REQUIRE(std::same_as<ex::completion_signatures_of_t<decltype(sender)>,
                                        ex::completion_signatures<ex::set_value_t(
                                            std::optional<native_error>, std::size_t)>>);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), receiver{})));
            auto operation = ex::connect(std::move(sender), receiver{&condition, &count});
            ex::start(operation);

            THEN("the condition and acknowledged count arrive through value completion") {
                CHECK(count == 4);
                CHECK(!condition);
            }
        }
    }

    GIVEN("a nonthrowing endpoint reporting a native condition") {
        object.condition = native_error::failed;

        WHEN("a read primitive is started") {
            auto sender = ex::io::async_read_some(object, buffer);
            STATIC_REQUIRE(std::same_as<ex::completion_signatures_of_t<decltype(sender)>,
                                        ex::completion_signatures<ex::set_value_t(
                                            std::optional<native_error>, std::size_t)>>);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), receiver{})));
            auto operation = ex::connect(std::move(sender), receiver{&condition, &count});
            ex::start(operation);

            THEN("the condition and acknowledged count arrive through value completion") {
                CHECK(count == 4);
                CHECK(condition == native_error::failed);
            }
        }

        WHEN("a write primitive is started") {
            auto sender = ex::io::async_write_some(object, buffer);
            STATIC_REQUIRE(std::same_as<ex::completion_signatures_of_t<decltype(sender)>,
                                        ex::completion_signatures<ex::set_value_t(
                                            std::optional<native_error>, std::size_t)>>);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), receiver{})));
            auto operation = ex::connect(std::move(sender), receiver{&condition, &count});
            ex::start(operation);

            THEN("the condition and acknowledged count arrive through value completion") {
                CHECK(count == 4);
                CHECK(condition == native_error::failed);
            }
        }
    }
}

SCENARIO("Native queries execute with exceptions disabled", "[io][no_exceptions]") {
    struct endpoint {
        unsigned factories = 0;
        auto async_query(std::span<const std::byte>, std::span<std::byte>) noexcept {
            ++factories;
            return ex::just();
        }
    } object;
    struct query_receiver {
        using receiver_concept = ex::receiver_tag;
        unsigned* values;
        void set_value() && noexcept { ++*values; }
    };
    std::array<std::byte, 4> buffer{};
    unsigned values = 0;

    GIVEN("a nonthrowing native query endpoint") {
        WHEN("a native query is started directly") {
            auto sender = ex::io::async_query(object, buffer, buffer);
            STATIC_REQUIRE(std::same_as<ex::completion_signatures_of_t<decltype(sender)>,
                                        ex::completion_signatures<ex::set_value_t()>>);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), query_receiver{})));
            CHECK(object.factories == 0);
            auto operation = ex::connect(std::move(sender), query_receiver{&values});
            CHECK(object.factories == 0);
            ex::start(operation);

            THEN("one native query completes without an exception path") {
                CHECK(values == 1);
                CHECK(object.factories == 1);
            }
        }

        WHEN("a native query follows an explicit predecessor") {
            auto sender =
                ex::io::async_query(ex::just(std::span{buffer}, std::span{buffer}), object);
            STATIC_REQUIRE(std::same_as<ex::completion_signatures_of_t<decltype(sender)>,
                                        ex::completion_signatures<ex::set_value_t()>>);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), query_receiver{})));
            CHECK(object.factories == 0);
            auto operation = ex::connect(std::move(sender), query_receiver{&values});
            CHECK(object.factories == 0);
            ex::start(operation);

            THEN("one native query completes without an exception path") {
                CHECK(values == 1);
                CHECK(object.factories == 1);
            }
        }

        WHEN("a native query follows a predecessor through the pipe form") {
            auto sender =
                ex::just(std::span{buffer}, std::span{buffer}) | ex::io::async_query(object);
            STATIC_REQUIRE(std::same_as<ex::completion_signatures_of_t<decltype(sender)>,
                                        ex::completion_signatures<ex::set_value_t()>>);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), query_receiver{})));
            CHECK(object.factories == 0);
            auto operation = ex::connect(std::move(sender), query_receiver{&values});
            CHECK(object.factories == 0);
            ex::start(operation);

            THEN("one native query completes without an exception path") {
                CHECK(values == 1);
                CHECK(object.factories == 1);
            }
        }
    }
}

SCENARIO("Exact transfers and composed queries execute with exceptions disabled",
         "[io][no_exceptions]") {
    struct result {
        unsigned values = 0, errors = 0;
        std::optional<native_error> error;
    } output;
    struct exact_receiver {
        using receiver_concept = ex::receiver_tag;
        result* output;
        void set_value() && noexcept { ++output->values; }
        void set_error(native_error error) && noexcept {
            ++output->errors;
            output->error = error;
        }
        void set_error(std::error_code) && noexcept { FAIL_CHECK("Unexpected progress error"); }
        void set_error(std::exception_ptr) && noexcept { FAIL_CHECK("Unexpected exception"); }
        void set_stopped() && noexcept { FAIL_CHECK("Unexpected stopped completion"); }
    };
    device object;
    std::array<std::byte, 4> buffer{};

    GIVEN("a nonthrowing endpoint acknowledging the complete buffer") {
        WHEN("an exact read is started") {
            auto sender = ex::io::async_read(object, buffer);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), exact_receiver{})));
            auto operation = ex::connect(std::move(sender), exact_receiver{&output});
            ex::start(operation);

            THEN("the exact operation completes once") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(!output.error);
            }
        }

        WHEN("an exact write is started") {
            auto sender = ex::io::async_write(object, buffer);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), exact_receiver{})));
            auto operation = ex::connect(std::move(sender), exact_receiver{&output});
            ex::start(operation);

            THEN("the exact operation completes once") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(!output.error);
            }
        }

        WHEN("an empty exact read is started") {
            auto sender = ex::io::async_read(object, {});
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), exact_receiver{})));
            auto operation = ex::connect(std::move(sender), exact_receiver{&output});
            ex::start(operation);

            THEN("the exact operation completes once") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(!output.error);
            }
        }

        WHEN("an exact read follows a buffer sender") {
            auto sender = ex::just(std::span{buffer}) | ex::io::async_read(object);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), exact_receiver{})));
            auto operation = ex::connect(std::move(sender), exact_receiver{&output});
            ex::start(operation);

            THEN("the exact operation completes once") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(!output.error);
            }
        }

        WHEN("an exact write follows a buffer sender") {
            auto sender = ex::just(std::span{buffer}) | ex::io::async_write(object);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), exact_receiver{})));
            auto operation = ex::connect(std::move(sender), exact_receiver{&output});
            ex::start(operation);

            THEN("the exact operation completes once") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(!output.error);
            }
        }

        WHEN("a composed query is started") {
            auto sender = ex::io::async_query(object, buffer, buffer);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), exact_receiver{})));
            auto operation = ex::connect(std::move(sender), exact_receiver{&output});
            ex::start(operation);

            THEN("the exact operation completes once") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(!output.error);
            }
        }

        WHEN("a composed query follows command and response buffers") {
            auto sender =
                ex::just(std::span{buffer}, std::span{buffer}) | ex::io::async_query(object);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), exact_receiver{})));
            auto operation = ex::connect(std::move(sender), exact_receiver{&output});
            ex::start(operation);

            THEN("the exact operation completes once") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(!output.error);
            }
        }
    }

    GIVEN("a nonthrowing endpoint acknowledging bytes with a native condition") {
        object.condition = native_error::failed;

        WHEN("an exact read is started") {
            auto sender = ex::io::async_read(object, buffer);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), exact_receiver{})));
            auto operation = ex::connect(std::move(sender), exact_receiver{&output});
            ex::start(operation);

            THEN("one typed error is delivered without throwing") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.error == native_error::failed);
            }
        }

        WHEN("an exact write is started") {
            auto sender = ex::io::async_write(object, buffer);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), exact_receiver{})));
            auto operation = ex::connect(std::move(sender), exact_receiver{&output});
            ex::start(operation);

            THEN("one typed error is delivered without throwing") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.error == native_error::failed);
            }
        }

        WHEN("a composed query is started") {
            auto sender = ex::io::async_query(object, buffer, buffer);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), exact_receiver{})));
            auto operation = ex::connect(std::move(sender), exact_receiver{&output});
            ex::start(operation);

            THEN("one typed error is delivered without throwing") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(output.error == native_error::failed);
            }
        }
    }
}
