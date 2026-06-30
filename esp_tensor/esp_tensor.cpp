#include <contracts>
#include <array>
#include <cstddef>
#include <meta>
#include <string_view>
#include <utility>

#include "esp_tensor.hpp"

#include "esp_log.h"
#include "esp_system.h"

namespace {

template <class Enum, std::size_t... Indices>
consteval auto enum_entries(std::index_sequence<Indices...> /*unused*/) {
    auto enumerators = std::meta::enumerators_of(^^Enum);
    return std::array{
        std::pair{std::meta::extract<Enum>(enumerators[Indices]),
                  std::meta::identifier_of(enumerators[Indices])}...,
    };
}

template <class Enum>
inline constexpr auto enum_entries_v =
    enum_entries<Enum>(std::make_index_sequence<std::meta::enumerators_of(^^Enum).size()>{});

[[nodiscard]] constexpr std::string_view enum_name(auto value) {
    using Enum = decltype(value);
    for (const auto& [candidate, name] : enum_entries_v<Enum>) {
        if (candidate == value) {
            return name;
        }
    }
    return "unknown";
}

[[nodiscard]] const char* safe_string(const char* value) { return value ? value : "<unknown>"; }

} // namespace

void handle_contract_violation(const std::contracts::contract_violation& violation) noexcept {
    static constexpr auto tag = "esp_tensor_contract";
    const auto location = violation.location();
    const auto kind = enum_name(violation.kind());
    const auto mode = enum_name(violation.mode());
    ESP_LOGE(tag,
             "ESP_CONTRACT_VIOLATION kind=%.*s mode=%.*s file=%s line=%lu function=%s "
             "predicate=%s",
             static_cast<int>(kind.size()), kind.data(), static_cast<int>(mode.size()), mode.data(),
             safe_string(location.file_name()), static_cast<unsigned long>(location.line()),
             safe_string(location.function_name()), safe_string(violation.comment()));
    esp_system_abort("ESP_CONTRACT_VIOLATION");
}

namespace esp::tensor {} // namespace esp::tensor
