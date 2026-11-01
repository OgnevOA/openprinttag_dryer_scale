// Minimal CBOR (RFC 8949) reader and writer covering the subset the
// OpenPrintTag specification uses: unsigned and negative integers, byte and
// text strings, arrays, maps (definite and indefinite length), tags (skipped),
// and half/single/double floats.
//
// Plain C++ with no Arduino dependencies so it can be built and tested on a
// desktop. Nothing here allocates: callers supply every buffer.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace opt {

enum CborMajor : uint8_t {
    CBOR_UINT = 0,
    CBOR_NEGINT = 1,
    CBOR_BYTES = 2,
    CBOR_TEXT = 3,
    CBOR_ARRAY = 4,
    CBOR_MAP = 5,
    CBOR_TAG = 6,
    CBOR_SIMPLE = 7,
};

// A decoded item header. `data` points past the header: at the payload for
// strings, at the first child for containers, or at the next item otherwise.
struct CborItem {
    uint8_t major = 0;
    bool indefinite = false;   // container or string of indefinite length
    bool isBreak = false;      // the 0xFF stop code
    bool isFloat = false;      // major 7 half/single/double
    uint64_t uval = 0;         // argument: value, byte length, or element count
    double dval = 0.0;         // decoded value when isFloat
    size_t start = 0;
    size_t data = 0;
};

// Maximum container nesting the skip logic will follow. The specification's
// deepest structure is a map holding an array, so this is generous; the limit
// exists to bound recursion on malformed tag data.
constexpr int kCborMaxDepth = 8;

bool cborRead(const uint8_t *buf, size_t len, size_t pos, CborItem &out);
bool cborSkip(const uint8_t *buf, size_t len, size_t pos, size_t &next);

// Numeric coercion: accepts unsigned, negative and float encodings.
bool cborNumber(const uint8_t *buf, size_t len, size_t pos, double &out);
bool cborUnsigned(const uint8_t *buf, size_t len, size_t pos, uint64_t &out);
bool cborInt(const uint8_t *buf, size_t len, size_t pos, int64_t &out);

// Copies a text string, always NUL terminating. Truncates to fit and reports
// truncation through `truncated` when the caller cares.
bool cborText(const uint8_t *buf, size_t len, size_t pos, char *dst,
              size_t dstSize, bool *truncated = nullptr);

bool cborBytes(const uint8_t *buf, size_t len, size_t pos, uint8_t *dst,
               size_t dstSize, size_t &outLen);

// Iterates integer-keyed map entries, yielding each value's byte range.
class CborMapIter {
public:
    CborMapIter(const uint8_t *buf, size_t len, size_t pos);

    bool valid() const { return valid_; }
    bool next(uint64_t &key, size_t &valuePos, size_t &valueEnd);

    // One past the final byte of the map. Only meaningful once next() has
    // returned false after a clean walk.
    size_t end() const { return cursor_; }
    bool truncated() const { return truncated_; }

private:
    const uint8_t *buf_;
    size_t len_;
    size_t cursor_ = 0;
    uint64_t remaining_ = 0;   // entries left, for definite-length maps
    bool indefinite_ = false;
    bool valid_ = false;
    bool truncated_ = false;
};

// Bounded, append-only writer. Overflow sets ok=false and is sticky, so a
// caller can emit a whole structure and check success once at the end.
class CborWriter {
public:
    CborWriter(uint8_t *buf, size_t cap) : buf_(buf), cap_(cap) {}

    void writeUnsigned(uint64_t value);
    void writeInt(int64_t value);

    // Integral values become integers (exact and compact); other values become
    // a half float when that stays within `kFloatTolerance` relative error, and
    // a single or double float otherwise.
    void writeNumber(double value);

    void writeBytes(const uint8_t *data, size_t length);
    void writeText(const char *text);
    void writeTextN(const char *text, size_t length);

    void writeArrayHeader(size_t count);
    void writeMapHeader(size_t count);
    void writeIndefiniteArray();
    void writeIndefiniteMap();
    void writeBreak();

    // Copies pre-encoded CBOR through verbatim, used to preserve fields the
    // firmware does not understand.
    void writeRaw(const uint8_t *data, size_t length);

    size_t length() const { return length_; }
    bool ok() const { return ok_; }
    size_t capacity() const { return cap_; }

private:
    void writeHead(uint8_t major, uint64_t argument);
    void put(uint8_t byte);

    uint8_t *buf_;
    size_t cap_;
    size_t length_ = 0;
    bool ok_ = true;
};

// Relative error accepted when narrowing a double to a half float.
constexpr double kFloatTolerance = 1e-3;

// Exposed for testing.
float halfToFloat(uint16_t half);
bool floatToHalf(double value, uint16_t &out);

}  // namespace opt
