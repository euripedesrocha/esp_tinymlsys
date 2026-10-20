// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include "esp_execution/just.hpp"
#include "esp_execution/let_value.hpp"
#include "esp_execution/repeat.hpp"
#include "esp_execution/scheduler.hpp"
#include "esp_execution/then.hpp"
#include "esp_execution/upon_error.hpp"
#include "esp_execution/upon_stopped.hpp"
#include "manual_scheduler_test_support.hpp"
#include "repeat_test_support.hpp"

namespace {
using namespace esp_execution_test;

template <class Scheduler> struct immediate_schedule_sender : decltype(execution::just()) {
    struct environment {
        Scheduler resource;

        auto query(execution::get_completion_scheduler_t<execution::set_value_t>) const noexcept
            -> Scheduler {
            return resource;
        }
    };

    Scheduler resource;

    auto get_env() const noexcept -> environment { return {resource}; }
};

struct immutable_scheduler {
    using scheduler_concept = execution::scheduler_tag;
    const int* const resource = nullptr;

    auto schedule() const noexcept {
        return immediate_schedule_sender<immutable_scheduler>{execution::just(), *this};
    }

    friend bool operator==(const immutable_scheduler&,
                           const immutable_scheduler&) noexcept = default;
};

// The inherited schedule sender identifies the base, not this scheduler type.
struct mismatched_scheduler : immutable_scheduler {};

struct missing_attributes_scheduler {
    using scheduler_concept = execution::scheduler_tag;
    auto schedule() const noexcept { return execution::just(); }
    friend bool operator==(missing_attributes_scheduler,
                           missing_attributes_scheduler) noexcept = default;
};

struct mutable_environment_scheduler {
    using scheduler_concept = execution::scheduler_tag;

    struct attributes {
        auto query(execution::get_completion_scheduler_t<execution::set_value_t>) noexcept
            -> mutable_environment_scheduler {
            return {};
        }
    };

    struct schedule_sender : decltype(execution::just()) {
        mutable attributes environment;
        auto get_env() const noexcept -> attributes& { return environment; }
    };

    auto schedule() const noexcept { return schedule_sender{execution::just(), {}}; }
    friend bool operator==(mutable_environment_scheduler,
                           mutable_environment_scheduler) noexcept = default;
};

enum class schedule_overload { not_called, const_lvalue, rvalue };

struct qualified_scheduler {
    using scheduler_concept = execution::scheduler_tag;
    schedule_overload* selected_overload;

    auto schedule() const& noexcept {
        *selected_overload = schedule_overload::const_lvalue;
        return immediate_schedule_sender<qualified_scheduler>{execution::just(), *this};
    }
    auto schedule() && noexcept(false) {
        *selected_overload = schedule_overload::rvalue;
        return immediate_schedule_sender<qualified_scheduler>{execution::just(), *this};
    }

    friend bool operator==(qualified_scheduler, qualified_scheduler) noexcept = default;
};

struct reference_attributes {
    immutable_scheduler value_resource;
    immutable_scheduler error_resource;
    immutable_scheduler stopped_resource;

    // Accept unknown tags so the public CPO, not this fixture, must reject them.
    template <class Tag>
    const immutable_scheduler& query(execution::get_completion_scheduler_t<Tag>) const noexcept {
        if constexpr (std::same_as<Tag, execution::set_value_t>) {
            return value_resource;
        } else if constexpr (std::same_as<Tag, execution::set_error_t>) {
            return error_resource;
        } else {
            return stopped_resource;
        }
    }
};

struct attributed_sender : deferred_sender {
    reference_attributes attributes;

    auto get_env() const noexcept -> const reference_attributes& { return attributes; }
};

struct throwing_attributes {
    immutable_scheduler query(auto) const noexcept(false) { return {}; }
};

struct mutable_attributes {
    immutable_scheduler query(auto) noexcept { return {}; }
};

struct invalid_attributes {
    int query(auto) const noexcept { return 0; }
};

template <class Env, class Tag>
concept has_completion_scheduler =
    std::invocable<decltype(execution::get_completion_scheduler<Tag>), const Env&>;
} // namespace

TEST_CASE("scheduler requires a matching success completion resource", "[scheduler][constraints]") {
    STATIC_REQUIRE(execution::scheduler<manual_scheduler>);
    STATIC_REQUIRE(execution::scheduler<immutable_scheduler>);
    STATIC_REQUIRE(execution::scheduler<const immutable_scheduler&>);
    STATIC_REQUIRE_FALSE(execution::scheduler<mismatched_scheduler>);
    STATIC_REQUIRE_FALSE(execution::scheduler<missing_attributes_scheduler>);
    STATIC_REQUIRE_FALSE(execution::scheduler<mutable_environment_scheduler>);
    STATIC_REQUIRE_FALSE(execution::scheduler<int>);
    // An immutable handle is intentionally not assignable: scheduling only needs copies.
    STATIC_REQUIRE_FALSE(std::copyable<immutable_scheduler>);
}

TEST_CASE("schedule preserves value category, result type and noexcept", "[scheduler][schedule]") {
    using result_type = immediate_schedule_sender<qualified_scheduler>;
    STATIC_REQUIRE(std::same_as<execution::schedule_result_t<qualified_scheduler>, result_type>);
    STATIC_REQUIRE(
        std::same_as<execution::schedule_result_t<const qualified_scheduler&>, result_type>);
    STATIC_REQUIRE(noexcept(execution::schedule(std::declval<const qualified_scheduler&>())));
    STATIC_REQUIRE_FALSE(noexcept(execution::schedule(std::declval<qualified_scheduler>())));

    auto selected = schedule_overload::not_called;
    const qualified_scheduler scheduler{.selected_overload = &selected};
    (void)execution::schedule(scheduler);
    REQUIRE(selected == schedule_overload::const_lvalue);

    selected = schedule_overload::not_called;
    (void)execution::schedule(qualified_scheduler{.selected_overload = &selected});
    REQUIRE(selected == schedule_overload::rvalue);
}

TEST_CASE("completion scheduler discovery accepts only const nonthrowing scheduler attributes",
          "[scheduler][attributes]") {
    STATIC_REQUIRE(has_completion_scheduler<reference_attributes, execution::set_value_t>);
    STATIC_REQUIRE(has_completion_scheduler<reference_attributes, execution::set_error_t>);
    STATIC_REQUIRE(has_completion_scheduler<reference_attributes, execution::set_stopped_t>);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<reference_attributes, int>);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<throwing_attributes, execution::set_value_t>);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<mutable_attributes, execution::set_value_t>);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<invalid_attributes, execution::set_value_t>);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<execution::empty_env, execution::set_value_t>);

    int value_resource = 0;
    int error_resource = 0;
    int stopped_resource = 0;
    const reference_attributes attributes{.value_resource = {.resource = &value_resource},
                                          .error_resource = {.resource = &error_resource},
                                          .stopped_resource = {.resource = &stopped_resource}};
    STATIC_REQUIRE(
        std::same_as<decltype(execution::get_completion_scheduler<execution::set_value_t>(
                         attributes)),
                     const immutable_scheduler&>);
    STATIC_REQUIRE(
        noexcept(execution::get_completion_scheduler<execution::set_value_t>(attributes)));
    REQUIRE(execution::get_completion_scheduler<execution::set_value_t>(attributes) ==
            attributes.value_resource);
    REQUIRE(execution::get_completion_scheduler<execution::set_error_t>(attributes) ==
            attributes.error_resource);
    REQUIRE(execution::get_completion_scheduler<execution::set_stopped_t>(attributes) ==
            attributes.stopped_resource);
}

TEST_CASE("schedule sender attributes preserve resource identity without starting work",
          "[scheduler][attributes][lifecycle]") {
    deferred_completion first;
    deferred_completion second;
    const manual_scheduler scheduler{&first};
    const manual_scheduler other{&second};
    const auto attributes = execution::get_env(execution::schedule(scheduler));
    auto recovered = execution::get_completion_scheduler<execution::set_value_t>(attributes);

    REQUIRE(recovered == scheduler);
    REQUIRE(recovered != other);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<decltype(attributes), execution::set_error_t>);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<decltype(attributes), execution::set_stopped_t>);
    REQUIRE(first.starts == 0);
    REQUIRE(second.starts == 0);

    result completion;
    auto operation =
        execution::connect(execution::schedule(recovered), result_receiver{.state = &completion});
    REQUIRE(first.starts == 0);
    execution::start(operation);
    REQUIRE(first.starts == 1);
    REQUIRE(completion.values == 0);
    first.deliver(completion_kind::value);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
    REQUIRE(second.starts == 0);
}

TEST_CASE("adaptors preserve unaffected scheduler attributes and hide dependent ones",
          "[scheduler][attributes]") {
    deferred_completion pending;
    int value_resource = 0;
    int error_resource = 0;
    int stopped_resource = 0;
    const reference_attributes attributes{.value_resource = {.resource = &value_resource},
                                          .error_resource = {.resource = &error_resource},
                                          .stopped_resource = {.resource = &stopped_resource}};
    const attributed_sender source{{.pending = &pending}, attributes};
    const auto transformed = source | execution::then([]() noexcept { return 42; });
    const auto recovered_error = source | execution::upon_error([](int) noexcept {});
    const auto recovered_stop = source | execution::upon_stopped([]() noexcept {});
    const auto dependent =
        source | execution::let_value([]() noexcept { return execution::just(); });
    const auto repeated = source | execution::repeat_n(0);

    REQUIRE(execution::get_completion_scheduler<execution::set_value_t>(
                execution::get_env(transformed)) == attributes.value_resource);
    REQUIRE(execution::get_completion_scheduler<execution::set_error_t>(
                execution::get_env(transformed)) == attributes.error_resource);
    REQUIRE(execution::get_completion_scheduler<execution::set_stopped_t>(
                execution::get_env(transformed)) == attributes.stopped_resource);
    REQUIRE(execution::get_completion_scheduler<execution::set_stopped_t>(
                execution::get_env(recovered_error)) == attributes.stopped_resource);
    REQUIRE(execution::get_completion_scheduler<execution::set_error_t>(
                execution::get_env(recovered_stop)) == attributes.error_resource);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<execution::env_of_t<decltype(recovered_error)>,
                                                  execution::set_error_t>);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<execution::env_of_t<decltype(recovered_stop)>,
                                                  execution::set_stopped_t>);
    STATIC_REQUIRE_FALSE(
        has_completion_scheduler<execution::env_of_t<decltype(dependent)>, execution::set_error_t>);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<execution::env_of_t<decltype(dependent)>,
                                                  execution::set_stopped_t>);
    STATIC_REQUIRE_FALSE(
        has_completion_scheduler<execution::env_of_t<decltype(repeated)>, execution::set_error_t>);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<execution::env_of_t<decltype(repeated)>,
                                                  execution::set_stopped_t>);
}

TEST_CASE("adaptors hide success attributes when the completion resource can change",
          "[scheduler][attributes]") {
    deferred_completion destination;
    const manual_scheduler scheduler{&destination};
    auto source = execution::schedule(scheduler);
    const auto recovered_error = source | execution::upon_error([](int) noexcept {});
    const auto recovered_stop = source | execution::upon_stopped([]() noexcept {});
    const auto dependent =
        source | execution::let_value([]() noexcept { return execution::just(); });
    const auto repeated = source | execution::repeat_n(0);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<execution::env_of_t<decltype(recovered_error)>,
                                                  execution::set_value_t>);
    STATIC_REQUIRE_FALSE(has_completion_scheduler<execution::env_of_t<decltype(recovered_stop)>,
                                                  execution::set_value_t>);
    STATIC_REQUIRE_FALSE(
        has_completion_scheduler<execution::env_of_t<decltype(dependent)>, execution::set_value_t>);
    STATIC_REQUIRE_FALSE(
        has_completion_scheduler<execution::env_of_t<decltype(repeated)>, execution::set_value_t>);
}
