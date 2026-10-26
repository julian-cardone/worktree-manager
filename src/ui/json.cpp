#include "ui/json.hpp"

#include "error.hpp"
#include "ui/format.hpp"

#include <cstdint>

namespace wt::ui {

namespace {

// The length of the UTF-8 sequence starting at `i`, or 0 if it isn't a valid
// one: overlong encodings, surrogates, and code points past U+10FFFF are
// rejected, as JSON parsers do.
std::size_t utf8_length(std::string_view text, std::size_t i) {
    const auto byte = [&](std::size_t k) {
        return static_cast<unsigned char>(text[k]);
    };
    const unsigned char lead = byte(i);
    if (lead < 0x80) {
        return 1;
    }
    // The sequence's length, the payload bits of its lead byte, and the
    // smallest code point that needs that many bytes.
    std::size_t length = 0;
    std::uint32_t code = 0;
    std::uint32_t min = 0;
    if ((lead & 0xe0) == 0xc0) {
        length = 2;
        code = lead & 0x1f;
        min = 0x80;
    } else if ((lead & 0xf0) == 0xe0) {
        length = 3;
        code = lead & 0x0f;
        min = 0x800;
    } else if ((lead & 0xf8) == 0xf0) {
        length = 4;
        code = lead & 0x07;
        min = 0x10000;
    } else {
        return 0;
    }
    if (i + length > text.size()) {
        return 0;
    }
    for (std::size_t k = i + 1; k < i + length; ++k) {
        if ((byte(k) & 0xc0) != 0x80) {
            return 0;
        }
        code = (code << 6) | (byte(k) & 0x3f);
    }
    if (code < min || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
        return 0;
    }
    return length;
}

}  // namespace

Json Json::string(std::string_view text) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t length = utf8_length(text, i);
        if (length == 0) {
            throw Error(ErrorKind::user,
                        "cannot print '" + printable(text) +
                            "' as JSON: it is not valid UTF-8");
        }
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20 || c == 0x7f) {
            // Escaped, not dropped, so the string is exact. DEL is escaped
            // too, as printable() replaces it, though JSON doesn't require it.
            out += "\\u00";
            out += kHex[c >> 4];
            out += kHex[c & 0xf];
        } else {
            out.append(text.substr(i, length));
        }
        i += length;
    }
    return Json(out + "\"");
}

Json Json::number(std::size_t value) {
    return Json(std::to_string(value));
}

Json Json::boolean(bool value) {
    return Json(value ? "true" : "false");
}

Json Json::null() {
    return Json("null");
}

Json Json::array(const std::vector<Json>& items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        out += (i == 0 ? "" : ",") + items[i].text_;
    }
    return Json(out + "]");
}

Json Json::object(
    std::initializer_list<std::pair<std::string_view, Json>> fields) {
    std::string out = "{";
    bool first = true;
    for (const auto& [name, value] : fields) {
        out += (first ? "" : ",") + string(name).text_ + ":" + value.text_;
        first = false;
    }
    return Json(out + "}");
}

Json Json::string_or_null(const std::optional<std::string>& value) {
    return value ? string(*value) : null();
}

}  // namespace wt::ui
