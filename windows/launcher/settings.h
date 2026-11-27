// settings.h -- what the launcher's choice settings in HKCU\Software\Win.apk mean, as read.
//
// A choice setting (Theme, Language, PhoneSize) is a small number the Settings page writes. A value the page never
// writes (another build's, or one typed into the registry) means the setting's default, 0, everywhere: what the
// launcher draws and what its Settings page shows as chosen are then the same thing, as Windows' own settings
// read an unknown value as their default.
//
// With no Windows call, so that test/launcher_state_test.cpp checks it.

#pragma once

#include <cstdint>

namespace winapk::launcher::settings {

// The choice a setting's value stands for, among `count`: the value itself when it is one of them, else 0.
constexpr uint32_t choice(uint32_t value, uint32_t count) { return value < count ? value : 0; }

// Theme: 0 as Windows, 1 light, 2 dark.
enum class Theme : uint32_t { kWindows = 0, kLight = 1, kDark = 2 };
constexpr uint32_t kThemes = 3;
constexpr Theme theme(uint32_t value) { return static_cast<Theme>(choice(value, kThemes)); }
constexpr bool theme_dark(uint32_t value, bool windows_dark) {
    return theme(value) == Theme::kDark || (theme(value) == Theme::kWindows && windows_dark);
}

// Language: 0 as Windows, 1 English, 2 Hebrew.
enum class Language : uint32_t { kWindows = 0, kEnglish = 1, kHebrew = 2 };
constexpr uint32_t kLanguages = 3;
constexpr Language language(uint32_t value) { return static_cast<Language>(choice(value, kLanguages)); }
constexpr bool language_hebrew(uint32_t value, bool windows_hebrew) {
    return language(value) == Language::kHebrew || (language(value) == Language::kWindows && windows_hebrew);
}

}  // namespace winapk::launcher::settings
