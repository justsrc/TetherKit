// Message-table consistency checks.
//
// The one reason this suite exists:
//
//   Tr()'s format strings come from a runtime table, so the compiler cannot
//   check placeholder/argument agreement -- the cost of using vformat instead
//   of std::format. A mismatch makes vformat throw std::format_error, which
//   FormatMessage swallows and degrades to un-substituted text, so the error
//   would quietly surface as "one weird log line" and could go unnoticed for
//   months.
//
//   This suite restores that guarantee: every message's placeholders must
//   parse cleanly, use contiguous indices starting at 0, and substitute with
//   real arguments. Adding a row to messages.def gets covered here
//   automatically, no manual registration.
//
// When a language is added, the cross-language parity checks return: each
// table must expose the same placeholder index set and presentation types as
// the English one (see the checklist in messages.def).
#include <cctype>
#include <cstddef>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "tetherkit/common/error.h"
#include "tetherkit/common/i18n.h"

namespace {

using tetherkit::Language;
using tetherkit::Msg;

constexpr std::size_t kMessageCount = static_cast<std::size_t>(Msg::kMessageCount);

/// Every replacement field found in one format string.
struct ParsedFormat {
  /// Index -> set of presentation-type chars used for that index.
  ///
  /// The presentation type is the last letter of the format spec (x / f / d
  /// ...); it decides what argument type must be passed. Width, alignment,
  /// and fill are deliberately not compared: a translation that adjusts
  /// column widths is perfectly legitimate and not a type hazard.
  std::map<std::size_t, std::string> types;
  /// Whether automatic and manual numbering were mixed in one string
  /// (std::format throws immediately).
  bool mixed_indexing = false;
  /// Whether a `{` was left unclosed.
  bool malformed = false;
};

/// Extracts the presentation-type char from a format spec. Empty when the
/// spec is empty or does not end in a letter.
[[nodiscard]] std::string PresentationType(std::string_view spec) {
  if (spec.empty()) {
    return {};
  }
  const char last = spec.back();
  return (std::isalpha(static_cast<unsigned char>(last)) != 0) ? std::string{last} : std::string{};
}

/// Parses a std::format string and collects the replacement fields.
///
/// Only the syntax subset our strings use: `{}`, `{n}`, `{:spec}`, `{n:spec}`,
/// plus the `{{` / `}}` escapes. Nested dynamic widths (`{:{}`) do not exist in
/// this table and count as malformed.
[[nodiscard]] ParsedFormat ParseFormat(std::string_view text) {
  ParsedFormat parsed;
  std::size_t auto_index = 0;
  bool saw_auto = false;
  bool saw_manual = false;

  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '}') {
      // A legal `}}` escape; skip the second one.
      if (i + 1 < text.size() && text[i + 1] == '}') {
        ++i;
      }
      continue;
    }
    if (text[i] != '{') {
      continue;
    }
    if (i + 1 < text.size() && text[i + 1] == '{') {
      ++i;  // `{{` escape
      continue;
    }

    const std::size_t close = text.find('}', i);
    if (close == std::string_view::npos) {
      parsed.malformed = true;
      break;
    }
    const std::string_view body = text.substr(i + 1, close - i - 1);
    i = close;

    // Split into "index" and "spec" halves.
    const std::size_t colon = body.find(':');
    const std::string_view index_text = body.substr(0, colon);
    const std::string_view spec =
        colon == std::string_view::npos ? std::string_view{} : body.substr(colon + 1);
    if (spec.find('{') != std::string_view::npos) {
      parsed.malformed = true;  // dynamic width has no business in this table
      continue;
    }

    std::size_t index = 0;
    if (index_text.empty()) {
      saw_auto = true;
      index = auto_index++;
    } else {
      saw_manual = true;
      for (const char c : index_text) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
          parsed.malformed = true;
          break;
        }
        index = (index * 10) + static_cast<std::size_t>(c - '0');
      }
    }
    parsed.types[index] = PresentationType(spec);
  }

  parsed.mixed_indexing = saw_auto && saw_manual;
  return parsed;
}

/// Readable label for failure output. The table does not store identifier
/// names (the X-macro expands to text only), so an index plus the start of the
/// English text is enough to locate the offending messages.def row.
[[nodiscard]] std::string Label(std::size_t index) {
  const std::string_view english = tetherkit::TextIn(Language::kEnglish, static_cast<Msg>(index));
  return std::format("message #{} (\"{}\")", index, english.substr(0, 48));
}

}  // namespace

TEST_SUITE("common.i18n") {
  TEST_CASE("language switch and table lookup") {
    const Language original = tetherkit::GetLanguage();

    // The selection machinery is process-wide and round-trips; today every
    // value resolves to the English table, but these calls exercise the same
    // path a second language will ride.
    tetherkit::SetLanguage(Language::kEnglish);
    CHECK(tetherkit::GetLanguage() == Language::kEnglish);
    CHECK(tetherkit::Text(Msg::kCoreRunStateRunning) == "running");

    tetherkit::SetLanguage(original);
  }

  TEST_CASE("Tr renders the current language and substitutes arguments") {
    const Language original = tetherkit::GetLanguage();
    tetherkit::SetLanguage(Language::kEnglish);

    const std::string text = tetherkit::Tr(Msg::kNetBpfBindFailed, "feth1");
    // The argument really was substituted, not left as an unreplaced field.
    CHECK(text.find("feth1") != std::string::npos);
    CHECK(text.find("{}") == std::string::npos);

    tetherkit::SetLanguage(original);
  }

  TEST_CASE("every message is non-empty") {
    for (std::size_t i = 0; i < kMessageCount; ++i) {
      const auto id = static_cast<Msg>(i);
      CAPTURE(i);
      CHECK_FALSE(tetherkit::TextIn(Language::kEnglish, id).empty());
    }
  }

  TEST_CASE("out-of-range Msg returns an empty string instead of crashing") {
    const auto out_of_range = static_cast<Msg>(kMessageCount);
    CHECK(tetherkit::Text(out_of_range).empty());
    CHECK(tetherkit::TextIn(Language::kEnglish, out_of_range).empty());
  }

  // This suite's core: the one check the compiler stopped doing, done entry by
  // entry.
  TEST_CASE("every message's placeholders are well-formed and contiguous") {
    for (std::size_t i = 0; i < kMessageCount; ++i) {
      const ParsedFormat parsed =
          ParseFormat(tetherkit::TextIn(Language::kEnglish, static_cast<Msg>(i)));

      CAPTURE(Label(i));
      CHECK_FALSE(parsed.malformed);
      // Mixing `{}` with `{0}` in one string makes std::format throw at
      // runtime; nothing at compile time can stop it.
      CHECK_FALSE(parsed.mixed_indexing);

      // Indices must be exactly 0..n-1: a gap still renders, but it means an
      // argument was silently ignored, which is almost always a mistake.
      std::size_t expected = 0;
      for (const auto& entry : parsed.types) {
        CHECK(entry.first == expected);
        ++expected;
      }
    }
  }

  TEST_CASE("ParseLanguage understands the established spellings") {
    Language language = Language::kEnglish;

    CHECK(tetherkit::ParseLanguage("en", &language));
    CHECK(language == Language::kEnglish);
    CHECK(tetherkit::ParseLanguage("EN_US", &language));
    CHECK(language == Language::kEnglish);
    CHECK(tetherkit::ParseLanguage("english", &language));
    CHECK(language == Language::kEnglish);

    // auto defers to the environment; only require it to succeed and yield a
    // valid language.
    CHECK(tetherkit::ParseLanguage("auto", &language));

    CHECK_FALSE(tetherkit::ParseLanguage("klingon", &language));
    CHECK_FALSE(tetherkit::ParseLanguage("", &language));
    CHECK_FALSE(tetherkit::ParseLanguage("zh", nullptr));

    // Chinese is fully out: no table, no spelling, not even as a parse.
    CHECK_FALSE(tetherkit::ParseLanguage("zh", &language));
    CHECK_FALSE(tetherkit::ParseLanguage("Chinese", &language));
  }

  TEST_CASE("locale strings resolve through the current language set") {
    // Documents today's behavior: with English as the only language, every
    // locale string resolves to English. When a language is added, the prefix
    // table goes into LanguageFromLocaleString() and these checks become
    // per-prefix.
    CHECK(tetherkit::LanguageFromLocaleString("zh_CN.UTF-8") == Language::kEnglish);
    CHECK(tetherkit::LanguageFromLocaleString("ZH-hant") == Language::kEnglish);
    CHECK(tetherkit::LanguageFromLocaleString("en_US.UTF-8") == Language::kEnglish);
    CHECK(tetherkit::LanguageFromLocaleString("C") == Language::kEnglish);
    CHECK(tetherkit::LanguageFromLocaleString("") == Language::kEnglish);
    CHECK(tetherkit::LanguageFromLocaleString("en_zh") == Language::kEnglish);
  }

  TEST_CASE("language tag") {
    CHECK(tetherkit::LanguageTag(Language::kEnglish) == "en");
  }

  TEST_CASE("error context separator is deterministic") {
    const Language original = tetherkit::GetLanguage();
    tetherkit::SetLanguage(Language::kEnglish);

    // Context() returns a view, so the Error must land in a named variable
    // before it is read; a temporary dies at the end of the statement and the
    // view dangles on the spot.
    const tetherkit::Error error = tetherkit::Error::Generic("inner").WithContext("outer");
    CHECK(error.Context().starts_with("outer: "));

    tetherkit::SetLanguage(original);
  }

}  // TEST_SUITE
