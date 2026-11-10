import Foundation

// User-facing strings and language switching for the GUI and the helper.
//
// Why not Localizable.strings / String(localized:):
//
//   The standard approach bundles .lproj directories, and this project ships
//   three independent artifact shapes:
//     * the bare executable that `swift build` produces (run during dev);
//     * the TetherKit.app assembled by Scripts/build-gui.sh;
//     * the **bare** helper executable that lives in
//       /Library/PrivilegedHelperTools.
//   The last one is fatal: no resource bundle sits next to the helper, and
//   none should, yet it still produces user-facing text (prompts, errors).
//   Going through bundles means the helper either speaks the build language
//   forever or the installer has to ship a second copy of the resources.
//
//   Compiling the strings into the binary has none of those problems: all
//   three shapes behave identically, install scripts stay untouched, and
//   there is no "installed on another machine, .lproj missing, everything
//   falls back to the development language" class of runtime failure. It also
//   mirrors the C++ side (include/tetherkit/common/messages.def): one mental
//   model.
//
//   The cost is Xcode's string-catalog editor -- this repo has no Xcode
//   project file anyway, so the cost is zero.
//
// Adding a message:
//
//   1. Add a case to `L10nKey` in LocalizedStrings.swift;
//   2. add its text to the `localizations` switch in the same file.
//   The switch is exhaustive: a missing entry is a compile error -- one notch
//   stronger than the C++ X-macro side.

/// The language preference the user can pick. `system` follows macOS.
public enum LanguagePreference: String, CaseIterable, Codable, Sendable {
    case system
    case english
}

/// The language actually in effect. The `system` preference resolves into one
/// of these through `L10n`.
public enum Language: String, CaseIterable, Codable, Sendable {
    case english

    /// Aligned with the C ABI's tk_language_t (TK_LANGUAGE_ENGLISH = 0). A
    /// language is one more enum value; on the wire, foreign values are
    /// ignored by every consumer, so callers keep working and stay in English.
    public var cValue: Int32 { 0 }
}

/// The string table and language state.
///
/// State is **process-wide**: logs and prompts come from multiple threads,
/// and thread-local state would split a single session's output across
/// languages. Reads far outnumber writes; one small lock is enough -- one
/// uncontended lock per table lookup is noise next to one SwiftUI render.
public enum L10n {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var storedPreference: LanguagePreference = .system
    nonisolated(unsafe) private static var storedLanguage: Language = resolve(.system)

    /// The language actually in effect.
    public static var language: Language {
        lock.lock()
        defer { lock.unlock() }
        return storedLanguage
    }

    /// The current preference (possibly `.system`).
    public static var preference: LanguagePreference {
        lock.lock()
        defer { lock.unlock() }
        return storedPreference
    }

    /// Applies a preference and returns the language that becomes effective.
    ///
    /// After taking the return value, the caller must **also** push it into
    /// libtetherkit (see `TetherKitLibrary.setLanguage`), or library logs and
    /// the interface language drift apart.
    @discardableResult
    public static func apply(_ preference: LanguagePreference) -> Language {
        let resolved = resolve(preference)
        lock.lock()
        storedPreference = preference
        storedLanguage = resolved
        lock.unlock()
        return resolved
    }

    /// macOS's current preferred language, mapped onto the languages we ship.
    ///
    /// Reads `Locale.preferredLanguages` rather than `Locale.current`: the
    /// latter follows the region-format setting (someone can have the region
    /// set to China while the interface language is English); the former is
    /// the actual list of interface languages.
    public static var systemLanguage: Language {
        localeLanguage(Locale.preferredLanguages.first ?? "en")
    }

    /// Maps a locale string (`"zh_CN.UTF-8"`, `"en_US"`, `"C"`, ...) to a
    /// language.
    ///
    /// English is the only shipped language, so every input resolves to it.
    /// This is where a language's prefix rule lands later (prefix match, case
    /// insensitive) -- kept separate from the preference walk so the rule can
    /// be unit-tested directly, exactly like LanguageFromLocaleString on the
    /// C++ side (see the checklist at the top of messages.def).
    static func localeLanguage(_ locale: String) -> Language {
        .english
    }

    private static func resolve(_ preference: LanguagePreference) -> Language {
        switch preference {
        case .system: return systemLanguage
        case .english: return .english
        }
    }

    /// The raw (unsubstituted) text of a message in the current language.
    public static func text(_ key: L10nKey) -> String {
        text(key, in: language)
    }

    /// The raw text of a message in the given language. Tests use it to check
    /// specific tables; this is the one function a new language changes (the
    /// single string becomes a per-language table again).
    public static func text(_ key: L10nKey, in language: Language) -> String {
        key.localizations
    }
}

/// Looks up a message and substitutes arguments per `String(format:)`.
///
/// A global function rather than `L10n.text(...)`: the two hundred-plus call
/// sites are mostly inside SwiftUI view bodies, where every character of
/// boilerplate crowds out the actual layout code. A one-letter name stays
/// unambiguous -- `L(` reads as "there is a translatable string here".
///
/// Placeholders are printf-style (`%@` strings, `%ld` integers, `%.1f`
/// floats). To reorder arguments, use positional forms: `%1$@`, `%2$ld`.
public func L(_ key: L10nKey, _ arguments: CVarArg...) -> String {
    let pattern = L10n.text(key)
    return arguments.isEmpty ? pattern : String(format: pattern, arguments: arguments)
}

/// Same, but in a specified language. For the helper -- it renders in the
/// language the App pushed, not its own process state (a root process has no
/// "user preference" concept).
public func L(_ key: L10nKey, in language: Language, _ arguments: CVarArg...) -> String {
    let pattern = L10n.text(key, in: language)
    return arguments.isEmpty ? pattern : String(format: pattern, arguments: arguments)
}
