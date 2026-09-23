#pragma once
// JSON object-key uniqueness for ordinary nested Desktop IPC payloads.
// Identical names in separate objects are valid; duplicates within one are not.
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace managed_json {
  inline nlohmann::json parse_object_keys(const std::string &line) {
    std::vector<std::set<std::string>> objects;
    const auto no_duplicates = [&](int, nlohmann::json::parse_event_t event, nlohmann::json &value) {
      using event_t = nlohmann::json::parse_event_t;
      if (event == event_t::object_start) objects.emplace_back();
      else if (event == event_t::object_end) objects.pop_back();
      else if (event == event_t::key && (objects.empty() || !objects.back().insert(value.get<std::string>()).second))
        throw std::invalid_argument("duplicate field");
      return true;
    };
    return nlohmann::json::parse(line, no_duplicates);
  }
}
