import XCTest

@testable import TethernetIPC

/// Consistency checks for the string table.
///
/// Why this suite exists (the one reason):
///
///   `L(...)` goes through `String(format:)`, and the format string comes
///   from a runtime table -- the compiler never sees it. `String(format:)`
///   does **not** complain about a mismatched placeholder: it quietly
///   reinterprets the argument in whatever type it read. `%@` hitting an
///   integer dereferences the integer as a pointer and crashes; `%ld` hitting
///   a string prints an address. Both are "a typo in a string -> runtime
///   crash on a user's machine", and nothing else in the build would catch
///   it.
///
///   So the guarantee is restored here. `L10nKey` is CaseIterable, so new
///   messages are covered automatically; no manual registration.
///
/// Cross-language parity checks return when a language is added: every table
/// must expose the same placeholder positions and conversions as the English
/// one (see the checklist at the top of messages.def).
final class LocalizationTests: XCTestCase {

    /// One replacement field: position (the explicit `%1$` number; implicit
    /// fields count in order of appearance) + conversion type.
    private struct Placeholder: Equatable, CustomStringConvertible {
        let position: Int
        let conversion: String

        var description: String { "#\(position):%\(conversion)" }
    }

    /// Parses a printf-style format string.
    ///
    /// Covers only the subset the table really uses: `%@`, `%ld`, `%lx`,
    /// `%f` and their width/precision/positional forms, plus the `%%` escape.
    private func placeholders(in text: String) -> [Placeholder] {
        var result: [Placeholder] = []
        var automatic = 0
        let characters = Array(text)
        var index = 0

        while index < characters.count {
            guard characters[index] == "%" else {
                index += 1
                continue
            }
            index += 1
            guard index < characters.count else { break }
            if characters[index] == "%" {  // `%%` escape
                index += 1
                continue
            }

            // Explicit position: `3$`
            var explicit: Int?
            var digits = ""
            var lookahead = index
            while lookahead < characters.count, characters[lookahead].isNumber {
                digits.append(characters[lookahead])
                lookahead += 1
            }
            if lookahead < characters.count, characters[lookahead] == "$", !digits.isEmpty {
                explicit = Int(digits)
                index = lookahead + 1
            }

            // Flags, width, precision -- all skipped, they do not affect the
            // argument type.
            while index < characters.count,
                  "0123456789.-+ #'".contains(characters[index]) {
                index += 1
            }
            // Length modifiers (l / ll / h / z) are part of the type and stay.
            var conversion = ""
            while index < characters.count, "lhzqjt".contains(characters[index]) {
                conversion.append(characters[index])
                index += 1
            }
            guard index < characters.count else { break }
            conversion.append(characters[index])
            index += 1

            automatic += 1
            result.append(Placeholder(position: explicit ?? automatic, conversion: conversion))
        }
        return result.sorted { $0.position < $1.position }
    }

    func testEveryKeyHasText() {
        for key in L10nKey.allCases {
            XCTAssertFalse(key.localizations.isEmpty, "\(key.rawValue) has no text")
        }
    }

    /// The core case.
    func testPlaceholderPositionsAreContiguous() {
        for key in L10nKey.allCases {
            let parsed = placeholders(in: key.localizations)
            for (offset, placeholder) in parsed.enumerated() {
                XCTAssertEqual(placeholder.position, offset + 1,
                               "\(key.rawValue) placeholder positions have a gap: \(parsed)")
            }
        }
    }

    /// Integers always use `%ld`: `%d` takes only the low 32 bits of a 64-bit
    /// argument, and Swift call sites pass Int.
    func testIntegerPlaceholdersUseLongModifier() {
        for key in L10nKey.allCases {
            for placeholder in placeholders(in: key.localizations) where
                ["d", "i", "u", "x", "X"].contains(placeholder.conversion) {
                XCTFail("""
                    \(key.rawValue) uses %\(placeholder.conversion); it should be \
                    %l\(placeholder.conversion) -- call sites pass Int (64-bit).
                    """)
            }
        }
    }

    func testLanguageResolution() {
        let original = L10n.preference
        defer { L10n.apply(original) }

        XCTAssertEqual(L10n.apply(.english), .english)
        XCTAssertEqual(L10n.text(.ok), "OK")

        // .system walks the same machinery; with one shipped language it
        // answers English for every locale input.
        XCTAssertEqual(L10n.apply(.system), .english)
        XCTAssertEqual(L10n.localeLanguage("zh_CN.UTF-8"), .english)
        XCTAssertEqual(L10n.localeLanguage("en_US.UTF-8"), .english)
        XCTAssertEqual(L10n.localeLanguage("C"), .english)
    }

    func testFormattingSubstitutesArguments() {
        let original = L10n.preference
        defer { L10n.apply(original) }

        L10n.apply(.english)
        let withArgument = L(.helperConnectFailed, "unreachable")
        XCTAssertTrue(withArgument.contains("unreachable"))
        XCTAssertFalse(withArgument.contains("%@"), "argument not substituted: \(withArgument)")

        // The positional entries must substitute by position, not by order of
        // appearance.
        let alias = L(.aliasCreated, in: .english, "/Applications/Tethernet", "/opt/Tethernet.app")
        XCTAssertTrue(alias.contains("/Applications/Tethernet"))
        XCTAssertTrue(alias.contains("/opt/Tethernet.app"))
    }

    /// Language tags must match the C ABI's tn_language_t -- if they drift,
    /// the GUI speaks one language and library logs speak another, with no
    /// error anywhere.
    func testCValueMatchesCABI() {
        XCTAssertEqual(Language.english.cValue, 0)
    }

    /// The language menu always shows options in their native name, never
    /// translated through the current interface language.
    func testLanguageNameIsNative() {
        XCTAssertEqual(L10nKey.languageEnglish.localizations, "English")
    }
}
