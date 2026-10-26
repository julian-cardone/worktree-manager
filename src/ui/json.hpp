#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wt::ui {

// A JSON value, serialized as it is built, so a document comes out compact and
// with its fields in the order they were given. wt's JSON output is part of its
// interface for agents and scripts, so documents are built field by field,
// never by serializing internal types.
class Json {
public:
    // Throws a user wt::Error if `text` isn't valid UTF-8: JSON can't carry
    // other bytes, and replacing them would hand scripts a path that doesn't
    // exist.
    static Json string(std::string_view text);
    static Json number(std::size_t value);
    static Json boolean(bool value);
    static Json null();
    static Json array(const std::vector<Json>& items);
    static Json object(
        std::initializer_list<std::pair<std::string_view, Json>> fields);

    // `value`, or null if there is none.
    static Json string_or_null(const std::optional<std::string>& value);

    const std::string& text() const { return text_; }

private:
    explicit Json(std::string text) : text_(std::move(text)) {}

    std::string text_;
};

}  // namespace wt::ui
