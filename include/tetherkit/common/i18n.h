// User-facing strings and runtime language selection.
//
// Why not gettext:
//
//   gettext would pull in libintl, require msgfmt at build time, and look up
//   .mo files by path at runtime. This project needs a fixed set of strings in
//   C++ covering the CLI, the shared library, and the GUI (the GUI switches the
//   language through the C ABI). One table compiled into the binary is the
//   cheapest correct option, and it cannot fail at runtime the way a missing
//   .mo file can.
//
// How the table is organized:
//
//   All strings live in messages.def, one row each:
//
//     TETHERKIT_MESSAGE(kCliUnknownOption, "unknown option \"{}\"")
//
//   The file is expanded by X-macros, producing the `Msg` enum (here) and the
//   message table (in i18n.cc) from the same source, so a missing entry is
//   structurally impossible. The one remaining error class is a placeholder
//   mismatch inside a string; tests/test_common_i18n.cc checks every string.
//
// Usage:
//
//   * Plain strings: Tr(Msg::kCliUnknownOption, argument) -- the arguments are
//     exactly std::format's; the result is a std::string.
//   * Logging: the TETHERKIT_INFO_TR family (logging.h) keeps the
//     do-not-evaluate-when-disabled laziness.
//   * Errors: Error::Generic(Tr(Msg::kFoo, x)).
//
// Never on hot paths:
//
//   Tr() runs vformat and returns std::string, so it allocates. Like error.h,
//   it must not be called in data-path hot code.
//
// Adding a language later:
//
//   The pipeline below is language-count agnostic: an enum, one lookup, one
//   parser, one env detector, and three mirrors (C ABI + Swift). The exact
//   five-touch checklist lives at the top of messages.def.
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace tetherkit {

/// The interface languages the library can serve.
///
/// Deliberately a closed enum, not an open language-tag string: one language
/// costs one column of strings in messages.def, and a compile-time "missing
/// column" failure beats a runtime fallback.
///
/// English is the only language shipped today. When a language is added, this
/// enum gains a value and TextIn() gains a table (see messages.def checklist).
enum class Language : std::uint8_t {
  kEnglish = 0,
};

/// Message identifiers. Values are generated from messages.def.
enum class Msg : std::uint16_t {
#define TETHERKIT_MESSAGE(id, en) id,
#include "tetherkit/common/messages.def"  // NOLINT(bugprone-suspicious-include)
#undef TETHERKIT_MESSAGE
  /// Sentinel: the number of messages. For table-length assertions and test
  /// loops only, not a real message.
  kMessageCount,
};

/// Sets the current language. Thread-safe, callable at any time.
///
/// Process-wide single state: logs and error strings come from multiple
/// threads, and per-thread state would split one session's output across
/// languages.
///
/// Wiring note: with one shipped language every value resolves to the same
/// table, but the API stays exactly as it will be when a second language
/// lands, so callers do not change again.
void SetLanguage(Language language) noexcept;

[[nodiscard]] Language GetLanguage() noexcept;

/// Infers the language from environment variables, checking `TETHERKIT_LANG`,
/// `LC_ALL`, `LC_MESSAGES`, and `LANG` in that order.
///
/// The first non-empty value wins (POSIX's LC_ALL > LC_MESSAGES > LANG order),
/// so stray later values cannot influence the result.
///
/// With one shipped language this always resolves to English; the chain stays
/// in place so a second language needs no new machinery, only the prefix rule
/// in LanguageFromLocaleString().
///
/// `sudo` may not pass any of these through depending on `env_keep`, which is
/// why the CLI also has `--lang`.
[[nodiscard]] Language DetectLanguageFromEnvironment() noexcept;

/// Maps a locale string (`"zh_CN.UTF-8"`, `"en_US"`, `"C"`, ...) to a language.
///
/// Today every input resolves to English because English is the only shipped
/// language. When a language is added, its prefix rule lands here (case
/// insensitive, prefix match). Kept as its own function so the rule can be
/// tested without touching the environment.
[[nodiscard]] Language LanguageFromLocaleString(std::string_view locale) noexcept;

/// Parses an `--lang` value. Accepts `en` / `en-US` / `english`, plus `auto`
/// and `system`, which defer to DetectLanguageFromEnvironment().
///
/// Spelling rules for removed or future languages deliberately return false,
/// so a language cannot be "half" supported: either it parses and has a table,
/// or it is an unknown value.
///
/// @return true and writes `out_language` on success; false and leaves
///         `out_language` untouched when unrecognized.
[[nodiscard]] bool ParseLanguage(std::string_view text, Language* out_language) noexcept;

/// Short tag for a language (`"en"`), used for echoes and logs.
[[nodiscard]] std::string_view LanguageTag(Language language) noexcept;

/// Returns the raw (unsubstituted) string for a message in the current
/// language.
///
/// An out-of-range `id` returns an empty string rather than crashing -- a
/// message-table failure must never take the driver down.
[[nodiscard]] std::string_view Text(Msg id) noexcept;

/// Returns the raw string for a message in the given language.
///
/// The language dispatch is the one function a new language changes (one more
/// table). Tests use it to address specific tables directly.
[[nodiscard]] std::string_view TextIn(Language language, Msg id) noexcept;

namespace detail {

/// Tr()'s implementation. A non-template function, so each call site does not
/// instantiate its own vformat copy.
///
/// Swallows std::format_error internally: when a string's placeholders do not
/// match its call site (only ever a string mistake), the result is the
/// un-substituted original text, not a process-killing throw.
[[nodiscard]] std::string FormatMessage(Msg id, std::format_args args) noexcept;

}  // namespace detail

/// Looks up a message in the current language and substitutes arguments.
/// Argument semantics are std::format's exactly.
///
/// The one difference from `std::format` is that the format string comes from
/// a runtime table, so the compiler cannot check placeholder/argument
/// agreement. tests/test_common_i18n.cc covers that gap.
template <typename... Args>
[[nodiscard]] std::string Tr(Msg id, const Args&... args) {
  return detail::FormatMessage(id, std::make_format_args(args...));
}

}  // namespace tetherkit
