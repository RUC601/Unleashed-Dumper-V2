#pragma once
#include <string>
#include <string_view>

namespace ud {
enum class Language { Chinese, English };
inline const char* local_text(Language language, const char* chinese, const char* english) {
    return language == Language::English ? english : chinese;
}
std::string diagnostic_text(Language language, std::string value);
}
