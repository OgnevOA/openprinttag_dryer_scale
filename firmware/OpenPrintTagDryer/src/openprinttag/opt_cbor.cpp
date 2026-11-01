#include "opt_cbor.h"

#include <math.h>
#include <string.h>

namespace opt {
namespace {

bool readArgument(const uint8_t *buf, size_t len, size_t pos, uint8_t info,
                  uint64_t &value, size_t &next) {
    if (info < 24) {
        value = info;
        next = pos + 1;
        return true;
    }

    size_t width = 0;
    switch (info) {
    case 24: width = 1; break;
    case 25: width = 2; break;
    case 26: width = 4; break;
    case 27: width = 8; break;
    default: return false;  // 28..30 are reserved, 31 handled by the caller
    }

    if (pos + 1 + width > len) {
        return false;
    }

    uint64_t accumulator = 0;
    for (size_t i = 0; i < width; ++i) {
        accumulator = (accumulator << 8) | buf[pos + 1 + i];
    }
    value = accumulator;
    next = pos + 1 + width;
    return true;
}

bool skipItem(const uint8_t *buf, size_t len, size_t pos, size_t &next,
              int depth) {
    if (depth > kCborMaxDepth) {
        return false;
    }

    CborItem item;
    if (!cborRead(buf, len, pos, item)) {
        return false;
    }
    if (item.isBreak) {
        next = item.data;
        return true;
    }

    switch (item.major) {
    case CBOR_UINT:
    case CBOR_NEGINT:
    case CBOR_SIMPLE:
        next = item.data;
        return true;

    case CBOR_BYTES:
    case CBOR_TEXT:
        if (!item.indefinite) {
            if (item.data + item.uval > len) {
                return false;
            }
            next = item.data + static_cast<size_t>(item.uval);
            return true;
        }
        // Indefinite string: a sequence of definite chunks ended by a break.
        {
            size_t cursor = item.data;
            while (cursor < len) {
                CborItem chunk;
                if (!cborRead(buf, len, cursor, chunk)) {
                    return false;
                }
                if (chunk.isBreak) {
                    next = chunk.data;
                    return true;
                }
                if (chunk.major != item.major || chunk.indefinite) {
                    return false;
                }
                if (chunk.data + chunk.uval > len) {
                    return false;
                }
                cursor = chunk.data + static_cast<size_t>(chunk.uval);
            }
            return false;
        }

    case CBOR_ARRAY:
    case CBOR_MAP: {
        const uint64_t perEntry = (item.major == CBOR_MAP) ? 2 : 1;
        size_t cursor = item.data;
        if (item.indefinite) {
            while (cursor < len) {
                CborItem probe;
                if (!cborRead(buf, len, cursor, probe)) {
                    return false;
                }
                if (probe.isBreak) {
                    next = probe.data;
                    return true;
                }
                for (uint64_t i = 0; i < perEntry; ++i) {
                    if (!skipItem(buf, len, cursor, cursor, depth + 1)) {
                        return false;
                    }
                }
            }
            return false;
        }
        // A definite-length container cannot claim more entries than there are
        // bytes left, which keeps a corrupt length from spinning.
        if (item.uval > len) {
            return false;
        }
        const uint64_t total = item.uval * perEntry;
        for (uint64_t i = 0; i < total; ++i) {
            if (!skipItem(buf, len, cursor, cursor, depth + 1)) {
                return false;
            }
        }
        next = cursor;
        return true;
    }

    case CBOR_TAG:
        return skipItem(buf, len, item.data, next, depth + 1);

    default:
        return false;
    }
}

}  // namespace

float halfToFloat(uint16_t half) {
    const uint16_t exponent = (half >> 10) & 0x1F;
    const uint16_t mantissa = half & 0x03FF;
    const float sign = (half & 0x8000) ? -1.0f : 1.0f;

    if (exponent == 0) {
        // Zero or subnormal.
        return sign * static_cast<float>(mantissa) * (1.0f / 16777216.0f);
    }
    if (exponent == 31) {
        return mantissa ? NAN : (sign * INFINITY);
    }
    return sign * ldexpf(static_cast<float>(mantissa + 1024),
                         static_cast<int>(exponent) - 25);
}

bool floatToHalf(double value, uint16_t &out) {
    if (isnan(value) || isinf(value)) {
        return false;
    }

    const float single = static_cast<float>(value);
    uint32_t bits = 0;
    memcpy(&bits, &single, sizeof(bits));

    const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000);
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127;
    const uint32_t mantissa = bits & 0x007FFFFF;

    if (single == 0.0f) {
        out = sign;
        return true;
    }
    if (exponent > 15 || exponent < -24) {
        return false;  // out of half-float range, including subnormal underflow
    }

    uint16_t candidate = 0;
    if (exponent < -14) {
        // Subnormal half: shift the implicit leading one into the mantissa.
        const uint32_t shift = static_cast<uint32_t>(-14 - exponent);
        const uint32_t full = mantissa | 0x00800000;
        candidate = static_cast<uint16_t>(sign | (full >> (shift + 13)));
    } else {
        candidate = static_cast<uint16_t>(
            sign | static_cast<uint16_t>((exponent + 15) << 10) |
            static_cast<uint16_t>(mantissa >> 13));
    }

    // Round to nearest by inspecting the bits the shift discarded, then accept
    // the result only if it stays within tolerance. Verifying instead of
    // trusting the arithmetic means a rounding mistake costs three extra bytes
    // rather than corrupting a value.
    const uint32_t dropped = mantissa & 0x1FFF;
    if (dropped > 0x1000 || (dropped == 0x1000 && (candidate & 1))) {
        ++candidate;
    }

    const double restored = static_cast<double>(halfToFloat(candidate));
    const double magnitude = fabs(value);
    const double error = fabs(restored - value);
    if (magnitude > 0.0 && (error / magnitude) > kFloatTolerance) {
        return false;
    }

    out = candidate;
    return true;
}

bool cborRead(const uint8_t *buf, size_t len, size_t pos, CborItem &out) {
    if (buf == nullptr || pos >= len) {
        return false;
    }

    const uint8_t initial = buf[pos];
    const uint8_t major = static_cast<uint8_t>(initial >> 5);
    const uint8_t info = static_cast<uint8_t>(initial & 0x1F);

    out = CborItem{};
    out.major = major;
    out.start = pos;

    if (info == 31) {
        if (major == CBOR_SIMPLE) {
            out.isBreak = true;
            out.data = pos + 1;
            return true;
        }
        if (major == CBOR_BYTES || major == CBOR_TEXT || major == CBOR_ARRAY ||
            major == CBOR_MAP) {
            out.indefinite = true;
            out.data = pos + 1;
            return true;
        }
        return false;
    }

    size_t next = 0;
    uint64_t argument = 0;
    if (!readArgument(buf, len, pos, info, argument, next)) {
        return false;
    }
    out.uval = argument;
    out.data = next;

    if (major == CBOR_SIMPLE) {
        switch (info) {
        case 25: {
            out.isFloat = true;
            out.dval = static_cast<double>(
                halfToFloat(static_cast<uint16_t>(argument)));
            break;
        }
        case 26: {
            const uint32_t bits = static_cast<uint32_t>(argument);
            float single = 0.0f;
            memcpy(&single, &bits, sizeof(single));
            out.isFloat = true;
            out.dval = static_cast<double>(single);
            break;
        }
        case 27: {
            double wide = 0.0;
            memcpy(&wide, &argument, sizeof(wide));
            out.isFloat = true;
            out.dval = wide;
            break;
        }
        default:
            break;  // false/true/null/undefined: header alone carries it
        }
    }

    return true;
}

bool cborSkip(const uint8_t *buf, size_t len, size_t pos, size_t &next) {
    return skipItem(buf, len, pos, next, 0);
}

bool cborNumber(const uint8_t *buf, size_t len, size_t pos, double &out) {
    CborItem item;
    if (!cborRead(buf, len, pos, item)) {
        return false;
    }
    if (item.isFloat) {
        out = item.dval;
        return true;
    }
    if (item.major == CBOR_UINT) {
        out = static_cast<double>(item.uval);
        return true;
    }
    if (item.major == CBOR_NEGINT) {
        out = -1.0 - static_cast<double>(item.uval);
        return true;
    }
    return false;
}

bool cborUnsigned(const uint8_t *buf, size_t len, size_t pos, uint64_t &out) {
    CborItem item;
    if (!cborRead(buf, len, pos, item)) {
        return false;
    }
    if (item.major == CBOR_UINT) {
        out = item.uval;
        return true;
    }
    // Accept a float that happens to hold a non-negative whole number, since
    // the specification lets any numeric field use a float encoding.
    if (item.isFloat && item.dval >= 0.0 && item.dval == floor(item.dval)) {
        out = static_cast<uint64_t>(item.dval);
        return true;
    }
    return false;
}

bool cborInt(const uint8_t *buf, size_t len, size_t pos, int64_t &out) {
    double value = 0.0;
    if (!cborNumber(buf, len, pos, value)) {
        return false;
    }
    out = static_cast<int64_t>(llround(value));
    return true;
}

bool cborText(const uint8_t *buf, size_t len, size_t pos, char *dst,
              size_t dstSize, bool *truncated) {
    if (dst == nullptr || dstSize == 0) {
        return false;
    }
    dst[0] = '\0';
    if (truncated != nullptr) {
        *truncated = false;
    }

    CborItem item;
    if (!cborRead(buf, len, pos, item) || item.major != CBOR_TEXT ||
        item.indefinite) {
        return false;
    }
    if (item.data + item.uval > len) {
        return false;
    }

    size_t copy = static_cast<size_t>(item.uval);
    if (copy > dstSize - 1) {
        copy = dstSize - 1;
        if (truncated != nullptr) {
            *truncated = true;
        }
    }
    memcpy(dst, buf + item.data, copy);
    dst[copy] = '\0';
    return true;
}

bool cborBytes(const uint8_t *buf, size_t len, size_t pos, uint8_t *dst,
               size_t dstSize, size_t &outLen) {
    outLen = 0;
    CborItem item;
    if (!cborRead(buf, len, pos, item) || item.major != CBOR_BYTES ||
        item.indefinite) {
        return false;
    }
    if (item.data + item.uval > len) {
        return false;
    }

    size_t copy = static_cast<size_t>(item.uval);
    if (copy > dstSize) {
        copy = dstSize;
    }
    if (dst != nullptr && copy > 0) {
        memcpy(dst, buf + item.data, copy);
    }
    outLen = copy;
    return true;
}

CborMapIter::CborMapIter(const uint8_t *buf, size_t len, size_t pos)
    : buf_(buf), len_(len) {
    CborItem item;
    if (!cborRead(buf, len, pos, item) || item.major != CBOR_MAP) {
        return;
    }
    indefinite_ = item.indefinite;
    remaining_ = item.indefinite ? 0 : item.uval;
    cursor_ = item.data;
    valid_ = true;
}

bool CborMapIter::next(uint64_t &key, size_t &valuePos, size_t &valueEnd) {
    if (!valid_) {
        return false;
    }

    if (indefinite_) {
        CborItem probe;
        if (!cborRead(buf_, len_, cursor_, probe)) {
            truncated_ = true;
            valid_ = false;
            return false;
        }
        if (probe.isBreak) {
            cursor_ = probe.data;
            valid_ = false;
            return false;
        }
    } else if (remaining_ == 0) {
        valid_ = false;
        return false;
    }

    // Keys are integers throughout the specification. Anything else means the
    // map is not one we can walk, so stop rather than guess.
    uint64_t parsedKey = 0;
    if (!cborUnsigned(buf_, len_, cursor_, parsedKey)) {
        truncated_ = true;
        valid_ = false;
        return false;
    }

    size_t afterKey = 0;
    if (!cborSkip(buf_, len_, cursor_, afterKey)) {
        truncated_ = true;
        valid_ = false;
        return false;
    }

    size_t afterValue = 0;
    if (!cborSkip(buf_, len_, afterKey, afterValue)) {
        truncated_ = true;
        valid_ = false;
        return false;
    }

    key = parsedKey;
    valuePos = afterKey;
    valueEnd = afterValue;
    cursor_ = afterValue;
    if (!indefinite_) {
        --remaining_;
    }
    return true;
}

void CborWriter::put(uint8_t byte) {
    if (!ok_) {
        return;
    }
    if (length_ >= cap_) {
        ok_ = false;
        return;
    }
    buf_[length_++] = byte;
}

void CborWriter::writeHead(uint8_t major, uint64_t argument) {
    const uint8_t base = static_cast<uint8_t>(major << 5);
    if (argument < 24) {
        put(static_cast<uint8_t>(base | argument));
    } else if (argument <= 0xFF) {
        put(static_cast<uint8_t>(base | 24));
        put(static_cast<uint8_t>(argument));
    } else if (argument <= 0xFFFF) {
        put(static_cast<uint8_t>(base | 25));
        put(static_cast<uint8_t>(argument >> 8));
        put(static_cast<uint8_t>(argument));
    } else if (argument <= 0xFFFFFFFFULL) {
        put(static_cast<uint8_t>(base | 26));
        for (int shift = 24; shift >= 0; shift -= 8) {
            put(static_cast<uint8_t>(argument >> shift));
        }
    } else {
        put(static_cast<uint8_t>(base | 27));
        for (int shift = 56; shift >= 0; shift -= 8) {
            put(static_cast<uint8_t>(argument >> shift));
        }
    }
}

void CborWriter::writeUnsigned(uint64_t value) {
    writeHead(CBOR_UINT, value);
}

void CborWriter::writeInt(int64_t value) {
    if (value < 0) {
        writeHead(CBOR_NEGINT, static_cast<uint64_t>(-(value + 1)));
    } else {
        writeHead(CBOR_UINT, static_cast<uint64_t>(value));
    }
}

void CborWriter::writeNumber(double value) {
    // Whole numbers go out as integers: exact, and shorter than any float form
    // for the magnitudes this format deals in.
    if (value == floor(value) && !isinf(value) &&
        value >= -9.2233720368547758e18 && value <= 1.8446744073709552e19) {
        writeInt(static_cast<int64_t>(value));
        return;
    }

    uint16_t half = 0;
    if (floatToHalf(value, half)) {
        put(static_cast<uint8_t>((CBOR_SIMPLE << 5) | 25));
        put(static_cast<uint8_t>(half >> 8));
        put(static_cast<uint8_t>(half));
        return;
    }

    const float single = static_cast<float>(value);
    if (static_cast<double>(single) == value) {
        uint32_t bits = 0;
        memcpy(&bits, &single, sizeof(bits));
        put(static_cast<uint8_t>((CBOR_SIMPLE << 5) | 26));
        for (int shift = 24; shift >= 0; shift -= 8) {
            put(static_cast<uint8_t>(bits >> shift));
        }
        return;
    }

    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    put(static_cast<uint8_t>((CBOR_SIMPLE << 5) | 27));
    for (int shift = 56; shift >= 0; shift -= 8) {
        put(static_cast<uint8_t>(bits >> shift));
    }
}

void CborWriter::writeBytes(const uint8_t *data, size_t length) {
    writeHead(CBOR_BYTES, length);
    for (size_t i = 0; i < length; ++i) {
        put(data[i]);
    }
}

void CborWriter::writeText(const char *text) {
    writeTextN(text, text ? strlen(text) : 0);
}

void CborWriter::writeTextN(const char *text, size_t length) {
    writeHead(CBOR_TEXT, length);
    for (size_t i = 0; i < length; ++i) {
        put(static_cast<uint8_t>(text[i]));
    }
}

void CborWriter::writeArrayHeader(size_t count) {
    writeHead(CBOR_ARRAY, count);
}

void CborWriter::writeMapHeader(size_t count) {
    writeHead(CBOR_MAP, count);
}

void CborWriter::writeIndefiniteArray() {
    put(static_cast<uint8_t>((CBOR_ARRAY << 5) | 31));
}

void CborWriter::writeIndefiniteMap() {
    put(static_cast<uint8_t>((CBOR_MAP << 5) | 31));
}

void CborWriter::writeBreak() {
    put(0xFF);
}

void CborWriter::writeRaw(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        put(data[i]);
    }
}

}  // namespace opt
