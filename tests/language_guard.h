// Test helper: pin the interface language within a scope, restore on exit.
//
// Why it exists:
//
//   Any test case along the lines of "the error message mentions XYZ"
//   depends on the current language. Language is process-wide state and its
//   default can be overwritten by other test cases, so without pinning, such
//   assertions depend on doctest's execution order -- the nastiest kind of
//   flaky failure to chase.
//
// So: any test case that asserts user-facing literal text opens with
// `const ScopedLanguage guard{Language::kEnglish};`.
#pragma once

#include "tetherkit/common/i18n.h"

namespace tetherkit::testing {

/// Switches the language within the scope; restores the entry value in the
/// destructor.
class ScopedLanguage {
 public:
  explicit ScopedLanguage(Language language) noexcept : previous_(GetLanguage()) {
    SetLanguage(language);
  }

  ScopedLanguage(const ScopedLanguage&) = delete;
  ScopedLanguage& operator=(const ScopedLanguage&) = delete;
  ScopedLanguage(ScopedLanguage&&) = delete;
  ScopedLanguage& operator=(ScopedLanguage&&) = delete;

  ~ScopedLanguage() { SetLanguage(previous_); }

 private:
  Language previous_;
};

}  // namespace tetherkit::testing
