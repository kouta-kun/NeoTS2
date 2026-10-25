// std::format for float and double on the PlayStation 2 toolchain, whose standard library has no floating-point
// to_chars and so no formatter for them (libstdc++ deletes it). Only what the engine writes is supported: an optional
// width, an optional precision and the type f, or nothing (the shortest text that reads back as the same number). Forced
// into every source file of a PS2 build by cmake/ps2.cmake. It also reads floating-point numbers for std::from_chars,
// which the library lacks for the same reason.
#pragma once

#if defined(__cplusplus) && __cplusplus >= 202002L

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <format>
#include <string>

namespace ts2::ps2 {

template <typename T>
struct FloatFormatter {
    constexpr std::format_parse_context::iterator parse(std::format_parse_context& context) {
        auto at = context.begin();
        const auto end = context.end();
        while (at != end && *at >= '0' && *at <= '9') {
            width_ = width_ * 10 + (*at - '0');
            ++at;
        }
        if (at != end && *at == '.') {
            ++at;
            precision_ = 0;
            while (at != end && *at >= '0' && *at <= '9') {
                precision_ = precision_ * 10 + (*at - '0');
                ++at;
            }
        }
        if (at != end && *at == 'f') {
            fixed_ = true;
            ++at;
        }
        if (at != end && *at != '}') {
            throw std::format_error("this floating-point format is not supported on the PlayStation 2");
        }
        return at;
    }

    std::format_context::iterator format(T value, std::format_context& context) const {
        char text[512];
        const double number = static_cast<double>(value);
        int length = 0;
        if (fixed_ || precision_ >= 0) {
            length = std::snprintf(text, sizeof(text), fixed_ ? "%.*f" : "%.*g", precision_ >= 0 ? std::min(precision_, 100) : 6, number);
        } else {
            // The fewest digits that read back as the same value, as std::format writes it.
            for (int digits = 1; digits <= 17; ++digits) {
                length = std::snprintf(text, sizeof(text), "%.*g", digits, number);
                if (std::strtod(text, nullptr) == number) {
                    break;
                }
            }
        }
        std::string out(text, static_cast<std::size_t>(std::max(length, 0)));
        if (out.size() < static_cast<std::size_t>(width_)) {
            out.insert(0, static_cast<std::size_t>(width_) - out.size(), ' ');
        }
        return std::copy(out.begin(), out.end(), context.out());
    }

private:
    int width_ = 0;
    int precision_ = -1;
    bool fixed_ = false;
};

} // namespace ts2::ps2

// Declared in std because that is where the call sites look: the library has no overload for the floating-point types here.
namespace std {

inline from_chars_result from_chars(const char* first, const char* last, float& value, chars_format = chars_format::general) noexcept {
    char text[128];
    const std::size_t length = static_cast<std::size_t>(last - first);
    // Like the real one, no leading space or plus sign is read.
    if (length == 0 || length >= sizeof(text) || *first == ' ' || *first == '\t' || *first == '+') {
        return {first, errc::invalid_argument};
    }
    std::memcpy(text, first, length);
    text[length] = '\0';
    char* end = nullptr;
    errno = 0;
    const float number = std::strtof(text, &end);
    if (end == text) {
        return {first, errc::invalid_argument};
    }
    value = number;
    return {first + (end - text), errno == ERANGE ? errc::result_out_of_range : errc{}};
}

inline from_chars_result from_chars(const char* first, const char* last, double& value, chars_format = chars_format::general) noexcept {
    float narrow = 0;
    const from_chars_result result = from_chars(first, last, narrow);
    value = static_cast<double>(narrow);
    return result;
}

} // namespace std

template <>
struct std::formatter<float, char> : ts2::ps2::FloatFormatter<float> {};
template <>
struct std::formatter<double, char> : ts2::ps2::FloatFormatter<double> {};

#endif
