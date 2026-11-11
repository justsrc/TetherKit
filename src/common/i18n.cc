#include "tethernet/common/i18n.h"

// ContextSeparator() is declared in error.h (which avoids pulling in
// <format>); including it here lets the compiler check that declaration and
// definition agree.
#include "tethernet/common/error.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

namespace tethernet {
namespace {

/// Message count. The table is fixed-length, so "added an enum value but no
/// string" fails at compile time.
constexpr std::size_t kMessageCount = static_cast<std::size_t>(Msg::kMessageCount);

/// Message table.
///
/// One column today. When a language is added: one more array right next to
/// this one, extracted from the new column, and a dispatch in TextIn() that
/// picks by Language (see the checklist in messages.def).
constexpr std::array<std::string_view, kMessageCount> kMessageTable{
#define TETHERNET_MESSAGE(id, en) en,
#include "tethernet/common/messages.def"  // NOLINT(bugprone-suspicious-include)
#undef TETHERNET_MESSAGE
};

/// Current language. Process-wide single state, atomic rather than mutexed:
/// every single message lookup reads it.
///
/// English stays the default: the CLI and the GUI overwrite it explicitly at
/// startup, and an unset language most likely belongs to a third-party
/// binding, for which English is more useful.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<Language> g_language{Language::kEnglish};

/// ASCII lowercasing. Only used for language-tag comparison, so locale rules
/// are irrelevant.
[[nodiscard]] char ToLowerAscii(char c) noexcept {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// Case-insensitive ASCII prefix test.
[[nodiscard]] bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (ToLowerAscii(text[i]) != ToLowerAscii(prefix[i])) {
      return false;
    }
  }
  return true;
}

/// Reads an environment variable; empty when unset or set to "".
[[nodiscard]] std::string_view ReadEnvironment(const char* name) noexcept {
  // getenv is flagged concurrency-mt-unsafe because it races with setenv.
  // Here it runs once at process startup, before any worker thread exists,
  // and the project never calls setenv.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return {};
  }
  return value;
}

}  // namespace

void SetLanguage(Language language) noexcept {
  g_language.store(language, std::memory_order_relaxed);
}

Language GetLanguage() noexcept {
  return g_language.load(std::memory_order_relaxed);
}

Language LanguageFromLocaleString(std::string_view /*locale*/) noexcept {
  // English is the only shipped language, so every locale string resolves to
  // it. This function exists as the place where a future language's prefix
  // rule lands (see the checklist in messages.def); keeping it separate from
  // the environment walk lets that rule be unit-tested directly.
  return Language::kEnglish;
}

Language DetectLanguageFromEnvironment() noexcept {
  // POSIX order: LC_ALL beats LC_MESSAGES beats LANG. TETHERNET_LANG comes
  // first so a user can change just this program's language without touching
  // the system locale.
  for (const char* name : {"TETHERNET_LANG", "LC_ALL", "LC_MESSAGES", "LANG"}) {
    const std::string_view value = ReadEnvironment(name);
    if (value.empty()) {
      continue;
    }
    // The first non-empty value decides; nothing later is consulted. That is
    // what makes LC_ALL actually outrank LANG instead of the other way
    // around.
    return LanguageFromLocaleString(value);
  }
  return Language::kEnglish;
}

bool ParseLanguage(std::string_view text, Language* out_language) noexcept {
  if (out_language == nullptr) {
    return false;
  }
  if (StartsWithIgnoreCase(text, "en") || StartsWithIgnoreCase(text, "english")) {
    *out_language = Language::kEnglish;
    return true;
  }
  if (StartsWithIgnoreCase(text, "auto") || StartsWithIgnoreCase(text, "system")) {
    *out_language = DetectLanguageFromEnvironment();
    return true;
  }
  return false;
}

std::string_view LanguageTag(Language /*language*/) noexcept {
  // English is the only shipped language. Keep the parameter so the signature
  // survives a second language unchanged.
  return "en";
}

std::string_view TextIn(Language /*language*/, Msg id) noexcept {
  // The language dispatch is here by design: with one table it always returns
  // it, and a second language adds a branch, not a signature change.
  const auto index = static_cast<std::size_t>(id);
  if (index >= kMessageCount) {
    return {};
  }
  return kMessageTable[index];
}

std::string_view Text(Msg id) noexcept {
  return TextIn(GetLanguage(), id);
}

namespace detail {

std::string FormatMessage(Msg id, std::format_args args) noexcept {
  const std::string_view pattern = Text(id);
  try {
    return std::vformat(pattern, args);
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Only reachable when a message's placeholders mismatch its call site
    // (a string-authoring mistake; the i18n tests catch it, but the runtime
    // must degrade rather than throw). Falling back to the un-substituted
    // original keeps a message present, just incomplete.
    return std::string{pattern};
  }
}

std::string_view ContextSeparator() noexcept {
  // English separators keep one leading space ("outer: inner"). The function
  // exists so a future language can pick its own punctuation.
  return ": ";
}

}  // namespace detail
}  // namespace tethernet
