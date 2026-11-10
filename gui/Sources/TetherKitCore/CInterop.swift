import CTetherKit
import Foundation

// C fixed-size char arrays are imported into Swift as tuples (`char[16]` -> a
// tuple of 16 CChars), which cannot be subscripted or converted to/from String
// directly. This is handled centrally here to avoid repeating unsafe pointer
// operations for every field.

extension String {
    /// Read a string from a C fixed-size char array (imported as a tuple).
    ///
    /// Does not use `String(cString:)`: that requires the buffer to be
    /// NUL-terminated, which would read past the end if the C side filled the
    /// entire buffer. This scans until NUL or the end, which is safe in both
    /// cases.
    init<T>(fixedCArray tuple: T) {
        var mutableCopy = tuple
        self = withUnsafeBytes(of: &mutableCopy) { raw in
            let bytes = raw.prefix { $0 != 0 }
            return String(decoding: bytes, as: UTF8.self)
        }
    }
}

/// Write a string into a C fixed-size char array, guaranteeing NUL termination.
///
/// Truncates when too long, and truncation falls on a UTF-8 character boundary
/// -- cutting on a raw byte would leave half a character at the end of the
/// buffer, which would read back on the C side as replacement characters. (C's
/// CopyText handles this the same way; both directions must take care of it.)
func setFixedCArray<T>(_ tuple: inout T, to string: String) {
    withUnsafeMutableBytes(of: &tuple) { raw in
        writeCString(string, into: raw)
    }
}

/// Write a string into a raw buffer with NUL termination and UTF-8-boundary-safe
/// truncation.
private func writeCString(_ string: String, into raw: UnsafeMutableRawBufferPointer) {
    guard raw.count > 0 else { return }
    for index in raw.indices { raw[index] = 0 }

    let utf8 = Array(string.utf8)
    var length = min(utf8.count, raw.count - 1)
    // length < utf8.count means truncation occurred; if the cut falls inside a
    // multibyte sequence (continuation bytes have the top two bits 0b10), walk
    // back to the start of that sequence.
    while length > 0, length < utf8.count, utf8[length] & 0xC0 == 0x80 {
        length -= 1
    }
    for index in 0..<length {
        raw[index] = utf8[index]
    }
}

/// Write a string into row `row` of a C 2D fixed-size array (for example
/// `char dns[4][46]`).
///
/// A 2D array is imported into Swift as a "tuple of tuples" and cannot be
/// subscripted, so it must be located by byte offset.
func setFixedCArrayRow<T>(_ tuple: inout T, row: Int, stride: Int, to string: String) {
    withUnsafeMutableBytes(of: &tuple) { raw in
        let start = row * stride
        guard start >= 0, start + stride <= raw.count else { return }
        writeCString(string, into: UnsafeMutableRawBufferPointer(rebasing: raw[start..<(start + stride)]))
    }
}

/// Read row `row` of a C 2D fixed-size array.
func fixedCArrayRow<T>(_ tuple: T, row: Int, stride: Int) -> String {
    var mutableCopy = tuple
    return withUnsafeBytes(of: &mutableCopy) { raw in
        let start = row * stride
        guard start >= 0, start + stride <= raw.count else { return "" }
        let slice = raw[start..<(start + stride)].prefix { $0 != 0 }
        return String(decoding: slice, as: UTF8.self)
    }
}

/// Render a 6-byte MAC tuple as "aa:bb:cc:dd:ee:ff"; returns an empty string
/// when all zeros.
func formatMAC<T>(_ tuple: T) -> String {
    var mutableCopy = tuple
    return withUnsafeBytes(of: &mutableCopy) { raw in
        guard raw.count >= 6, raw.prefix(6).contains(where: { $0 != 0 }) else { return "" }
        return raw.prefix(6).map { String(format: "%02x", $0) }.joined(separator: ":")
    }
}
