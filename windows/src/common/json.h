// Minimal JSON for the settings file: one object of scalar values. Nested objects and arrays
// are parsed (so the file stays readable) but stored as null.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>

namespace vr::json {

using Value = std::variant<std::nullptr_t, bool, double, std::string>;
using Object = std::map<std::string, Value>;

// nullopt if the text is not a JSON object.
std::optional<Object> parseObject(const std::string& text);
// Pretty-printed, keys sorted (std::map order).
std::string write(const Object& object);

}  // namespace vr::json
