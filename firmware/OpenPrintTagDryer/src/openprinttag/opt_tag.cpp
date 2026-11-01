#include "opt_tag.h"

#include <string.h>

#include "opt_cbor.h"

namespace opt {
namespace {

constexpr size_t kMaxMainEncoded = 384;

size_t alignUp(size_t value, size_t alignment) {
    const size_t remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

bool readNdefRecord(const uint8_t *buf, size_t len, size_t pos,
                    size_t &typePos, size_t &typeLen, size_t &payloadPos,
                    size_t &payloadLen, uint8_t &tnf, bool &isLast,
                    size_t &next) {
    if (pos >= len) {
        return false;
    }

    const uint8_t flags = buf[pos];
    const bool shortRecord = (flags & 0x10) != 0;
    const bool hasIdLength = (flags & 0x08) != 0;
    tnf = static_cast<uint8_t>(flags & 0x07);
    isLast = (flags & 0x40) != 0;

    size_t cursor = pos + 1;
    if (cursor >= len) {
        return false;
    }
    typeLen = buf[cursor++];

    if (shortRecord) {
        if (cursor >= len) {
            return false;
        }
        payloadLen = buf[cursor++];
    } else {
        if (cursor + 4 > len) {
            return false;
        }
        payloadLen = (static_cast<size_t>(buf[cursor]) << 24) |
                     (static_cast<size_t>(buf[cursor + 1]) << 16) |
                     (static_cast<size_t>(buf[cursor + 2]) << 8) |
                     static_cast<size_t>(buf[cursor + 3]);
        cursor += 4;
    }

    size_t idLen = 0;
    if (hasIdLength) {
        if (cursor >= len) {
            return false;
        }
        idLen = buf[cursor++];
    }

    typePos = cursor;
    cursor += typeLen;
    cursor += idLen;
    if (cursor > len) {
        return false;
    }

    payloadPos = cursor;
    if (payloadPos + payloadLen > len) {
        return false;
    }
    next = payloadPos + payloadLen;
    return true;
}

// Expands a URI record's payload back into text. NDEF stores the common
// prefixes as a single leading byte, and only the four `optBuildImage` knows how
// to put back are expanded here: leaving the output empty for the rest is what
// tells a caller that rebuilding the tag would change the link, which is worse
// than refusing to rebuild it. A URI too long for the buffer is left empty for
// the same reason rather than truncated into a different address.
void captureUri(const uint8_t *image, size_t payloadPos, size_t payloadLen,
                char *out, size_t cap) {
    out[0] = '\0';
    if (payloadLen < 1) {
        return;
    }
    const char *prefix = nullptr;
    switch (image[payloadPos]) {
        case 0x00: prefix = ""; break;
        case 0x01: prefix = "http://www."; break;
        case 0x02: prefix = "https://www."; break;
        case 0x03: prefix = "http://"; break;
        case 0x04: prefix = "https://"; break;
        default: return;
    }

    const size_t restLen = payloadLen - 1;
    const size_t prefixLen = strlen(prefix);
    if (prefixLen + restLen + 1 > cap) {
        return;
    }
    memcpy(out, prefix, prefixLen);
    memcpy(out + prefixLen, image + payloadPos + 1, restLen);
    out[prefixLen + restLen] = '\0';
}

// Copies an integer-keyed field's value out of a map, dispatching on the type
// the specification assigns to that key.
void decodeMainField(const uint8_t *region, size_t len, uint16_t key,
                     size_t valuePos, OptMainData &out) {
    double number = 0.0;
    int64_t integer = 0;
    uint64_t unsignedValue = 0;

    switch (key) {
    case main_key::kMaterialClass:
        if (cborUnsigned(region, len, valuePos, unsignedValue)) {
            out.materialClass = static_cast<uint16_t>(unsignedValue);
            out.hasMaterialClass = true;
        }
        return;
    case main_key::kMaterialType:
        if (cborUnsigned(region, len, valuePos, unsignedValue)) {
            out.materialType = static_cast<uint16_t>(unsignedValue);
            out.hasMaterialType = true;
        }
        return;
    case main_key::kWriteProtection:
        if (cborUnsigned(region, len, valuePos, unsignedValue)) {
            out.writeProtection = static_cast<uint16_t>(unsignedValue);
            out.hasWriteProtection = true;
        }
        return;
    case main_key::kGtin:
        if (cborUnsigned(region, len, valuePos, unsignedValue)) {
            out.gtin = unsignedValue;
            out.hasGtin = true;
        }
        return;

    case main_key::kMaterialName:
        cborText(region, len, valuePos, out.materialName,
                 sizeof(out.materialName));
        return;
    case main_key::kMaterialAbbreviation:
        cborText(region, len, valuePos, out.materialAbbreviation,
                 sizeof(out.materialAbbreviation));
        return;
    case main_key::kBrandName:
        cborText(region, len, valuePos, out.brandName, sizeof(out.brandName));
        return;
    case main_key::kBrandSpecificInstanceId:
        cborText(region, len, valuePos, out.brandSpecificInstanceId,
                 sizeof(out.brandSpecificInstanceId));
        return;

    case main_key::kManufacturedDate:
        if (cborUnsigned(region, len, valuePos, unsignedValue)) {
            out.manufacturedDate = static_cast<uint32_t>(unsignedValue);
            out.hasManufacturedDate = true;
        }
        return;
    case main_key::kExpirationDate:
        if (cborUnsigned(region, len, valuePos, unsignedValue)) {
            out.expirationDate = static_cast<uint32_t>(unsignedValue);
            out.hasExpirationDate = true;
        }
        return;

    case main_key::kNominalNettoFullWeight:
        if (cborNumber(region, len, valuePos, number)) {
            out.nominalNettoFullWeight = static_cast<float>(number);
            out.hasNominalNettoFullWeight = true;
        }
        return;
    case main_key::kActualNettoFullWeight:
        if (cborNumber(region, len, valuePos, number)) {
            out.actualNettoFullWeight = static_cast<float>(number);
            out.hasActualNettoFullWeight = true;
        }
        return;
    case main_key::kEmptyContainerWeight:
        if (cborNumber(region, len, valuePos, number)) {
            out.emptyContainerWeight = static_cast<float>(number);
            out.hasEmptyContainerWeight = true;
        }
        return;
    case main_key::kNominalFullLength:
        if (cborNumber(region, len, valuePos, number)) {
            out.nominalFullLength = static_cast<float>(number);
            out.hasNominalFullLength = true;
        }
        return;
    case main_key::kActualFullLength:
        if (cborNumber(region, len, valuePos, number)) {
            out.actualFullLength = static_cast<float>(number);
            out.hasActualFullLength = true;
        }
        return;
    case main_key::kDensity:
        if (cborNumber(region, len, valuePos, number)) {
            out.density = static_cast<float>(number);
            out.hasDensity = true;
        }
        return;
    case main_key::kFilamentDiameter:
        if (cborNumber(region, len, valuePos, number)) {
            out.filamentDiameter = static_cast<float>(number);
            out.hasFilamentDiameter = true;
        }
        return;

    case main_key::kInstanceUuid:
    case main_key::kPackageUuid:
    case main_key::kMaterialUuid:
    case main_key::kBrandUuid: {
        uint8_t *target = out.instanceUuid;
        bool *present = &out.hasInstanceUuid;
        if (key == main_key::kPackageUuid) {
            target = out.packageUuid;
            present = &out.hasPackageUuid;
        } else if (key == main_key::kMaterialUuid) {
            target = out.materialUuid;
            present = &out.hasMaterialUuid;
        } else if (key == main_key::kBrandUuid) {
            target = out.brandUuid;
            present = &out.hasBrandUuid;
        }
        size_t copied = 0;
        // Exactly 16 bytes or nothing: a short value is not a UUID, and half of
        // one is worse than none because it would be written back as fact.
        // Anything else falls through to be counted as unrecognised.
        if (cborBytes(region, len, valuePos, target, kUuidSize, copied) &&
            copied == kUuidSize) {
            *present = true;
            return;
        }
        break;
    }

    case main_key::kPrimaryColor: {
        size_t copied = 0;
        if (cborBytes(region, len, valuePos, out.primaryColor,
                      sizeof(out.primaryColor), copied) &&
            copied >= 3) {
            out.primaryColorLen = static_cast<uint8_t>(copied);
            if (copied == 3) {
                out.primaryColor[3] = 0xFF;
            }
            out.hasPrimaryColor = true;
        }
        return;
    }

    case main_key::kTags: {
        CborItem item;
        if (!cborRead(region, len, valuePos, item) ||
            item.major != CBOR_ARRAY) {
            return;
        }
        size_t cursor = item.data;
        uint64_t remaining = item.indefinite ? UINT64_MAX : item.uval;
        while (remaining > 0 && cursor < len) {
            CborItem element;
            if (!cborRead(region, len, cursor, element)) {
                return;
            }
            if (element.isBreak) {
                return;
            }
            uint64_t tagValue = 0;
            if (cborUnsigned(region, len, cursor, tagValue)) {
                if (out.tagCount < kMaxMaterialTags) {
                    out.tags[out.tagCount++] = static_cast<uint16_t>(tagValue);
                } else {
                    out.tagsTruncated = true;
                }
            }
            if (!cborSkip(region, len, cursor, cursor)) {
                return;
            }
            if (!item.indefinite) {
                --remaining;
            }
        }
        return;
    }

    default:
        break;
    }

    // Small signed integers: temperatures and container dimensions.
    struct IntField {
        uint16_t key;
        int16_t *value;
        bool *present;
    };
    const IntField intFields[] = {
        {main_key::kMinPrintTemperature, &out.minPrintTemperature,
         &out.hasMinPrintTemperature},
        {main_key::kMaxPrintTemperature, &out.maxPrintTemperature,
         &out.hasMaxPrintTemperature},
        {main_key::kPreheatTemperature, &out.preheatTemperature,
         &out.hasPreheatTemperature},
        {main_key::kMinBedTemperature, &out.minBedTemperature,
         &out.hasMinBedTemperature},
        {main_key::kMaxBedTemperature, &out.maxBedTemperature,
         &out.hasMaxBedTemperature},
        {main_key::kMinChamberTemperature, &out.minChamberTemperature,
         &out.hasMinChamberTemperature},
        {main_key::kMaxChamberTemperature, &out.maxChamberTemperature,
         &out.hasMaxChamberTemperature},
        {main_key::kChamberTemperature, &out.chamberTemperature,
         &out.hasChamberTemperature},
        {main_key::kDryingTemperature, &out.dryingTemperature,
         &out.hasDryingTemperature},
        {main_key::kDryingTime, &out.dryingTime, &out.hasDryingTime},
        {main_key::kContainerWidth, &out.containerWidth,
         &out.hasContainerWidth},
        {main_key::kContainerOuterDiameter, &out.containerOuterDiameter,
         &out.hasContainerOuterDiameter},
        {main_key::kContainerInnerDiameter, &out.containerInnerDiameter,
         &out.hasContainerInnerDiameter},
        {main_key::kContainerHoleDiameter, &out.containerHoleDiameter,
         &out.hasContainerHoleDiameter},
    };
    for (const IntField &field : intFields) {
        if (field.key == key) {
            if (cborInt(region, len, valuePos, integer)) {
                *field.value = static_cast<int16_t>(integer);
                *field.present = true;
            }
            return;
        }
    }

    // Keys the specification defines but this firmware has no use for
    // (viscosities, secondary colours, certifications, ...) land here alongside
    // genuinely unknown ones. Either way they are counted, never rewritten, so
    // this count is the measure of what a full rewrite would discard.
    if (out.unknownFieldCount < 255) {
        ++out.unknownFieldCount;
    }
}

void decodeAuxField(const uint8_t *region, size_t len, uint16_t key,
                    size_t valuePos, OptAuxData &out) {
    double number = 0.0;
    uint64_t unsignedValue = 0;

    switch (key) {
    case aux_key::kConsumedWeight:
        if (cborNumber(region, len, valuePos, number)) {
            out.consumedWeight = static_cast<float>(number);
            out.hasConsumedWeight = true;
        }
        return;
    case aux_key::kWorkgroup:
        cborText(region, len, valuePos, out.workgroup, sizeof(out.workgroup));
        return;
    case aux_key::kStorageLocation:
        cborText(region, len, valuePos, out.storageLocation,
                 sizeof(out.storageLocation));
        return;
    case aux_key::kPurchaseTime:
        if (cborUnsigned(region, len, valuePos, unsignedValue)) {
            out.purchaseTime = static_cast<uint32_t>(unsignedValue);
            out.hasPurchaseTime = true;
        }
        return;
    case aux_key::kPurchasePrice:
        if (cborNumber(region, len, valuePos, number)) {
            out.purchasePrice = static_cast<float>(number);
            out.hasPurchasePrice = true;
        }
        return;
    case aux_key::kPurchaseCurrency:
        cborText(region, len, valuePos, out.purchaseCurrency,
                 sizeof(out.purchaseCurrency));
        return;
    default:
        if (out.unknownFieldCount < 255) {
            ++out.unknownFieldCount;
        }
        return;
    }
}

bool isKnownAuxKey(uint64_t key) {
    switch (key) {
    case aux_key::kConsumedWeight:
    case aux_key::kWorkgroup:
    case aux_key::kGeneralPurposeRangeUser:
    case aux_key::kLastStirTime:
    case aux_key::kStorageLocation:
    case aux_key::kPurchaseTime:
    case aux_key::kPurchasePrice:
    case aux_key::kPurchaseCurrency:
        return true;
    default:
        return false;
    }
}

// Writes the main section as a definite-length map in ascending key order.
// Returns the number of key/value pairs written.
size_t encodeMain(const OptMainData &main, CborWriter &writer, bool countOnly) {
    size_t count = 0;

    auto beginField = [&](uint16_t key) {
        ++count;
        if (!countOnly) {
            writer.writeUnsigned(key);
        }
    };

    // Keys 0-3, so they lead.
    const struct {
        uint16_t key;
        const uint8_t *value;
        bool present;
    } uuids[] = {
        {main_key::kInstanceUuid, main.instanceUuid, main.hasInstanceUuid},
        {main_key::kPackageUuid, main.packageUuid, main.hasPackageUuid},
        {main_key::kMaterialUuid, main.materialUuid, main.hasMaterialUuid},
        {main_key::kBrandUuid, main.brandUuid, main.hasBrandUuid},
    };
    for (const auto &uuid : uuids) {
        if (uuid.present) {
            beginField(uuid.key);
            if (!countOnly) {
                writer.writeBytes(uuid.value, kUuidSize);
            }
        }
    }

    if (main.hasGtin) {
        beginField(main_key::kGtin);
        if (!countOnly) {
            writer.writeUnsigned(main.gtin);
        }
    }
    if (main.brandSpecificInstanceId[0] != '\0') {
        beginField(main_key::kBrandSpecificInstanceId);
        if (!countOnly) {
            writer.writeText(main.brandSpecificInstanceId);
        }
    }
    if (main.hasMaterialClass) {
        beginField(main_key::kMaterialClass);
        if (!countOnly) {
            writer.writeUnsigned(main.materialClass);
        }
    }
    if (main.hasMaterialType) {
        beginField(main_key::kMaterialType);
        if (!countOnly) {
            writer.writeUnsigned(main.materialType);
        }
    }
    if (main.materialName[0] != '\0') {
        beginField(main_key::kMaterialName);
        if (!countOnly) {
            writer.writeText(main.materialName);
        }
    }
    if (main.brandName[0] != '\0') {
        beginField(main_key::kBrandName);
        if (!countOnly) {
            writer.writeText(main.brandName);
        }
    }
    if (main.hasWriteProtection) {
        beginField(main_key::kWriteProtection);
        if (!countOnly) {
            writer.writeUnsigned(main.writeProtection);
        }
    }
    if (main.hasManufacturedDate) {
        beginField(main_key::kManufacturedDate);
        if (!countOnly) {
            writer.writeUnsigned(main.manufacturedDate);
        }
    }
    if (main.hasExpirationDate) {
        beginField(main_key::kExpirationDate);
        if (!countOnly) {
            writer.writeUnsigned(main.expirationDate);
        }
    }
    if (main.hasNominalNettoFullWeight) {
        beginField(main_key::kNominalNettoFullWeight);
        if (!countOnly) {
            writer.writeNumber(main.nominalNettoFullWeight);
        }
    }
    if (main.hasActualNettoFullWeight) {
        beginField(main_key::kActualNettoFullWeight);
        if (!countOnly) {
            writer.writeNumber(main.actualNettoFullWeight);
        }
    }
    if (main.hasEmptyContainerWeight) {
        beginField(main_key::kEmptyContainerWeight);
        if (!countOnly) {
            writer.writeNumber(main.emptyContainerWeight);
        }
    }
    if (main.hasPrimaryColor) {
        beginField(main_key::kPrimaryColor);
        if (!countOnly) {
            const size_t length = main.primaryColorLen >= 3
                                      ? main.primaryColorLen
                                      : static_cast<size_t>(4);
            writer.writeBytes(main.primaryColor, length);
        }
    }
    if (main.tagCount > 0) {
        beginField(main_key::kTags);
        if (!countOnly) {
            writer.writeArrayHeader(main.tagCount);
            for (uint8_t i = 0; i < main.tagCount; ++i) {
                writer.writeUnsigned(main.tags[i]);
            }
        }
    }
    if (main.hasDensity) {
        beginField(main_key::kDensity);
        if (!countOnly) {
            writer.writeNumber(main.density);
        }
    }
    if (main.hasFilamentDiameter) {
        beginField(main_key::kFilamentDiameter);
        if (!countOnly) {
            writer.writeNumber(main.filamentDiameter);
        }
    }
    if (main.hasMinPrintTemperature) {
        beginField(main_key::kMinPrintTemperature);
        if (!countOnly) {
            writer.writeInt(main.minPrintTemperature);
        }
    }
    if (main.hasMaxPrintTemperature) {
        beginField(main_key::kMaxPrintTemperature);
        if (!countOnly) {
            writer.writeInt(main.maxPrintTemperature);
        }
    }
    if (main.hasPreheatTemperature) {
        beginField(main_key::kPreheatTemperature);
        if (!countOnly) {
            writer.writeInt(main.preheatTemperature);
        }
    }
    if (main.hasMinBedTemperature) {
        beginField(main_key::kMinBedTemperature);
        if (!countOnly) {
            writer.writeInt(main.minBedTemperature);
        }
    }
    if (main.hasMaxBedTemperature) {
        beginField(main_key::kMaxBedTemperature);
        if (!countOnly) {
            writer.writeInt(main.maxBedTemperature);
        }
    }
    if (main.hasMinChamberTemperature) {
        beginField(main_key::kMinChamberTemperature);
        if (!countOnly) {
            writer.writeInt(main.minChamberTemperature);
        }
    }
    if (main.hasMaxChamberTemperature) {
        beginField(main_key::kMaxChamberTemperature);
        if (!countOnly) {
            writer.writeInt(main.maxChamberTemperature);
        }
    }
    if (main.hasChamberTemperature) {
        beginField(main_key::kChamberTemperature);
        if (!countOnly) {
            writer.writeInt(main.chamberTemperature);
        }
    }
    if (main.hasContainerWidth) {
        beginField(main_key::kContainerWidth);
        if (!countOnly) {
            writer.writeInt(main.containerWidth);
        }
    }
    if (main.hasContainerOuterDiameter) {
        beginField(main_key::kContainerOuterDiameter);
        if (!countOnly) {
            writer.writeInt(main.containerOuterDiameter);
        }
    }
    if (main.hasContainerInnerDiameter) {
        beginField(main_key::kContainerInnerDiameter);
        if (!countOnly) {
            writer.writeInt(main.containerInnerDiameter);
        }
    }
    if (main.hasContainerHoleDiameter) {
        beginField(main_key::kContainerHoleDiameter);
        if (!countOnly) {
            writer.writeInt(main.containerHoleDiameter);
        }
    }
    if (main.materialAbbreviation[0] != '\0') {
        beginField(main_key::kMaterialAbbreviation);
        if (!countOnly) {
            writer.writeText(main.materialAbbreviation);
        }
    }
    if (main.hasNominalFullLength) {
        beginField(main_key::kNominalFullLength);
        if (!countOnly) {
            writer.writeNumber(main.nominalFullLength);
        }
    }
    if (main.hasActualFullLength) {
        beginField(main_key::kActualFullLength);
        if (!countOnly) {
            writer.writeNumber(main.actualFullLength);
        }
    }
    if (main.hasDryingTemperature) {
        beginField(main_key::kDryingTemperature);
        if (!countOnly) {
            writer.writeInt(main.dryingTemperature);
        }
    }
    if (main.hasDryingTime) {
        beginField(main_key::kDryingTime);
        if (!countOnly) {
            writer.writeInt(main.dryingTime);
        }
    }

    return count;
}

size_t encodeMeta(uint16_t auxOffset, bool hasAux, uint8_t *out, size_t cap) {
    CborWriter writer(out, cap);
    // Only the auxiliary region offset is emitted. Main follows the meta
    // section immediately and runs to the next region, which the specification
    // defines as the default when the offset and size fields are absent.
    writer.writeMapHeader(hasAux ? 1 : 0);
    if (hasAux) {
        writer.writeUnsigned(meta_key::kAuxRegionOffset);
        writer.writeUnsigned(auxOffset);
    }
    return writer.ok() ? writer.length() : 0;
}

}  // namespace

const char *optStatusText(OptStatus status) {
    switch (status) {
    case OptStatus::Ok: return "ok";
    case OptStatus::Truncated: return "truncated data";
    case OptStatus::NoCapabilityContainer: return "no capability container";
    case OptStatus::NoNdefTlv: return "no NDEF TLV";
    case OptStatus::NoOptRecord: return "no OpenPrintTag NDEF record";
    case OptStatus::BadMetaSection: return "bad meta section";
    case OptStatus::BadMainSection: return "bad main section";
    case OptStatus::BadAuxSection: return "bad auxiliary section";
    case OptStatus::RegionOutOfRange: return "region outside payload";
    case OptStatus::BufferTooSmall: return "buffer too small";
    }
    return "unknown";
}

OptStatus optParseMain(const uint8_t *region, size_t len, OptMainData &out) {
    out = OptMainData{};
    CborMapIter iter(region, len, 0);
    if (!iter.valid()) {
        return OptStatus::BadMainSection;
    }

    uint64_t key = 0;
    size_t valuePos = 0;
    size_t valueEnd = 0;
    while (iter.next(key, valuePos, valueEnd)) {
        decodeMainField(region, len, static_cast<uint16_t>(key), valuePos, out);
    }
    return iter.truncated() ? OptStatus::BadMainSection : OptStatus::Ok;
}

OptStatus optParseAux(const uint8_t *region, size_t len, OptAuxData &out) {
    out = OptAuxData{};
    CborMapIter iter(region, len, 0);
    if (!iter.valid()) {
        return OptStatus::BadAuxSection;
    }

    uint64_t key = 0;
    size_t valuePos = 0;
    size_t valueEnd = 0;
    while (iter.next(key, valuePos, valueEnd)) {
        decodeAuxField(region, len, static_cast<uint16_t>(key), valuePos, out);
    }
    return iter.truncated() ? OptStatus::BadAuxSection : OptStatus::Ok;
}

OptStatus optParse(const uint8_t *image, size_t len, OptTag &out) {
    out = OptTag{};
    if (image == nullptr || len < 8) {
        return OptStatus::Truncated;
    }

    if (image[0] != kCcMagicE1 && image[0] != kCcMagicE2) {
        return OptStatus::NoCapabilityContainer;
    }
    const size_t ccLength = (image[0] == kCcMagicE2) ? 8 : 4;
    out.regions.ccCapacity = static_cast<uint16_t>(image[2] * 8u);

    // Walk the TLV blocks looking for the NDEF one. It is not required to come
    // first, and other TLV types may precede it.
    size_t cursor = ccLength;
    size_t ndefValue = 0;
    size_t ndefLength = 0;
    bool foundNdef = false;
    while (cursor < len) {
        const uint8_t type = image[cursor];
        if (type == kTlvTerminator) {
            break;
        }
        if (type == 0x00) {
            ++cursor;  // padding
            continue;
        }
        if (cursor + 1 >= len) {
            return OptStatus::Truncated;
        }

        size_t valueLength = image[cursor + 1];
        size_t valueStart = cursor + 2;
        if (valueLength == 0xFF) {
            if (cursor + 3 >= len) {
                return OptStatus::Truncated;
            }
            valueLength = (static_cast<size_t>(image[cursor + 2]) << 8) |
                          static_cast<size_t>(image[cursor + 3]);
            valueStart = cursor + 4;
        }
        if (valueStart + valueLength > len) {
            return OptStatus::Truncated;
        }

        if (type == kTlvNdef) {
            out.regions.ndefTlvStart = static_cast<uint16_t>(cursor);
            ndefValue = valueStart;
            ndefLength = valueLength;
            foundNdef = true;
            break;
        }
        cursor = valueStart + valueLength;
    }

    if (!foundNdef) {
        return OptStatus::NoNdefTlv;
    }

    // Find the OpenPrintTag record inside the NDEF message. Other records may
    // come first, typically a URI pointing at the manufacturer's spool page.
    const size_t messageEnd = ndefValue + ndefLength;
    size_t recordPos = ndefValue;
    bool foundRecord = false;
    while (recordPos < messageEnd) {
        size_t typePos = 0;
        size_t typeLen = 0;
        size_t payloadPos = 0;
        size_t payloadLen = 0;
        uint8_t tnf = 0;
        bool isLast = false;
        size_t next = 0;
        if (!readNdefRecord(image, messageEnd, recordPos, typePos, typeLen,
                            payloadPos, payloadLen, tnf, isLast, next)) {
            return OptStatus::Truncated;
        }

        if (tnf == 0x01 && typeLen == 1 && image[typePos] == 'U') {
            out.hasUriRecord = true;
            captureUri(image, payloadPos, payloadLen, out.uri, sizeof(out.uri));
        }
        if (tnf == 0x02 && typeLen == kMimeTypeLen &&
            memcmp(image + typePos, kMimeType, kMimeTypeLen) == 0) {
            out.regions.payloadStart = static_cast<uint16_t>(payloadPos);
            out.regions.payloadSize = static_cast<uint16_t>(payloadLen);
            foundRecord = true;
            break;
        }

        if (isLast) {
            break;
        }
        recordPos = next;
    }

    if (!foundRecord) {
        return OptStatus::NoOptRecord;
    }

    const uint8_t *payload = image + out.regions.payloadStart;
    const size_t payloadSize = out.regions.payloadSize;

    // Meta section: one CBOR map at the very start of the payload.
    size_t metaEnd = 0;
    if (!cborSkip(payload, payloadSize, 0, metaEnd)) {
        return OptStatus::BadMetaSection;
    }
    out.regions.metaSize = static_cast<uint16_t>(metaEnd);

    uint16_t mainOffset = static_cast<uint16_t>(metaEnd);
    bool haveMainOffset = false;
    uint16_t mainSize = 0;
    bool haveMainSize = false;
    uint16_t auxOffset = 0;
    uint16_t auxSize = 0;
    bool haveAuxSize = false;

    {
        CborMapIter iter(payload, payloadSize, 0);
        if (!iter.valid()) {
            return OptStatus::BadMetaSection;
        }
        uint64_t key = 0;
        size_t valuePos = 0;
        size_t valueEnd = 0;
        while (iter.next(key, valuePos, valueEnd)) {
            uint64_t value = 0;
            if (!cborUnsigned(payload, payloadSize, valuePos, value)) {
                continue;
            }
            switch (key) {
            case meta_key::kMainRegionOffset:
                mainOffset = static_cast<uint16_t>(value);
                haveMainOffset = true;
                break;
            case meta_key::kMainRegionSize:
                mainSize = static_cast<uint16_t>(value);
                haveMainSize = true;
                break;
            case meta_key::kAuxRegionOffset:
                auxOffset = static_cast<uint16_t>(value);
                out.regions.hasAux = true;
                break;
            case meta_key::kAuxRegionSize:
                auxSize = static_cast<uint16_t>(value);
                haveAuxSize = true;
                break;
            default:
                break;
            }
        }
        if (iter.truncated()) {
            return OptStatus::BadMetaSection;
        }
    }

    if (!haveMainOffset) {
        mainOffset = static_cast<uint16_t>(metaEnd);
    }
    if (!haveMainSize) {
        // Runs to the next region, or to the end of the payload.
        if (out.regions.hasAux && auxOffset > mainOffset) {
            mainSize = static_cast<uint16_t>(auxOffset - mainOffset);
        } else {
            mainSize = static_cast<uint16_t>(payloadSize - mainOffset);
        }
    }
    if (out.regions.hasAux && !haveAuxSize) {
        if (auxOffset > payloadSize) {
            return OptStatus::RegionOutOfRange;
        }
        auxSize = static_cast<uint16_t>(payloadSize - auxOffset);
    }

    if (static_cast<size_t>(mainOffset) + mainSize > payloadSize) {
        return OptStatus::RegionOutOfRange;
    }
    if (out.regions.hasAux &&
        static_cast<size_t>(auxOffset) + auxSize > payloadSize) {
        return OptStatus::RegionOutOfRange;
    }

    out.regions.mainOffset = mainOffset;
    out.regions.mainSize = mainSize;
    out.regions.auxOffset = auxOffset;
    out.regions.auxSize = auxSize;

    const OptStatus mainStatus =
        optParseMain(payload + mainOffset, mainSize, out.main);
    if (mainStatus != OptStatus::Ok) {
        return mainStatus;
    }

    if (out.regions.hasAux) {
        out.auxStatus = optParseAux(payload + auxOffset, auxSize, out.aux);
    }

    return OptStatus::Ok;
}

bool optMainRegionSpan(const uint8_t *image, size_t len, const OptTag &tag,
                       const uint8_t *&start, size_t &length) {
    const size_t offset = tag.regions.mainAbsolute();
    if (image == nullptr || tag.regions.mainSize == 0 ||
        offset + tag.regions.mainSize > len) {
        return false;
    }

    // The declared region usually runs to the next region, so it can end in
    // padding. Walking the map gives the byte after the last field, and padding
    // inside a copied region would not be valid CBOR.
    CborMapIter iter(image + offset, tag.regions.mainSize, 0);
    if (!iter.valid()) {
        return false;
    }
    uint64_t key = 0;
    size_t valuePos = 0;
    size_t valueEnd = 0;
    while (iter.next(key, valuePos, valueEnd)) {
    }
    if (iter.truncated() || iter.end() == 0) {
        return false;
    }

    start = image + offset;
    length = iter.end();
    return true;
}

OptStatus optBuildImage(const OptBuildRequest &request, uint8_t *out,
                        size_t outCap, size_t &outLen, OptRegions *regions) {
    outLen = 0;
    if (out == nullptr) {
        return OptStatus::BufferTooSmall;
    }

    // The capability container states the length in 8-byte units, so a tag's
    // usable size gets rounded down: a 316 B SLIX2 carries a 312 B image.
    size_t imageSize = (request.tagCapacity / 8) * 8;
    if (imageSize > outCap || imageSize > kMaxTagImage || imageSize < 32) {
        return OptStatus::BufferTooSmall;
    }

    size_t uriPayloadLen = 0;
    size_t uriRecordLen = 0;
    const char *uriRest = nullptr;
    uint8_t uriAbbreviation = 0x00;
    if (request.uri != nullptr && request.uri[0] != '\0') {
        // NDEF abbreviates the common URI prefixes into a single byte.
        struct Prefix {
            uint8_t code;
            const char *text;
        };
        const Prefix prefixes[] = {
            {0x04, "https://"},
            {0x03, "http://"},
            {0x02, "https://www."},
            {0x01, "http://www."},
        };
        uriRest = request.uri;
        for (const Prefix &prefix : prefixes) {
            const size_t length = strlen(prefix.text);
            if (strncmp(request.uri, prefix.text, length) == 0) {
                uriAbbreviation = prefix.code;
                uriRest = request.uri + length;
                break;
            }
        }
        uriPayloadLen = 1 + strlen(uriRest);
        if (uriPayloadLen > 255) {
            return OptStatus::BufferTooSmall;
        }
        uriRecordLen = 4 + uriPayloadLen;  // flags, type len, payload len, 'U'
    }

    // Pick the record and TLV length encodings that leave the most payload.
    size_t bestPayload = 0;
    bool bestShortRecord = false;
    bool bestShortTlv = false;
    bool found = false;
    for (int recordVariant = 0; recordVariant < 2; ++recordVariant) {
        const bool shortRecord = recordVariant == 1;
        const size_t recordHeader = 1 + 1 + (shortRecord ? 1u : 4u) +
                                    kMimeTypeLen;
        for (int tlvVariant = 0; tlvVariant < 2; ++tlvVariant) {
            const bool shortTlv = tlvVariant == 1;
            const size_t tlvHeader = shortTlv ? 2 : 4;
            const size_t overhead = 4 + tlvHeader + uriRecordLen +
                                    recordHeader + 1;
            if (overhead + request.minAuxSize > imageSize) {
                continue;
            }
            const size_t payload = imageSize - overhead;
            const size_t messageLength = uriRecordLen + recordHeader + payload;
            if (shortRecord && payload > 255) {
                continue;
            }
            if (shortTlv && messageLength > 254) {
                continue;
            }
            if (!found || payload > bestPayload) {
                bestPayload = payload;
                bestShortRecord = shortRecord;
                bestShortTlv = shortTlv;
                found = true;
            }
        }
    }
    if (!found) {
        return OptStatus::BufferTooSmall;
    }

    const size_t recordHeaderLen = 1 + 1 + (bestShortRecord ? 1u : 4u) +
                                   kMimeTypeLen;
    const size_t tlvHeaderLen = bestShortTlv ? 2 : 4;
    const size_t payloadSize = bestPayload;
    const size_t payloadStart = 4 + tlvHeaderLen + uriRecordLen +
                                recordHeaderLen;

    // Encode the main section once; its length drives where the aux region can
    // start. A verbatim region is copied instead, which is how a rebuild keeps
    // fields this firmware cannot represent.
    uint8_t mainBuffer[kMaxMainEncoded];
    size_t mainLength = 0;
    if (request.mainVerbatim != nullptr) {
        if (request.mainVerbatimLen == 0 ||
            request.mainVerbatimLen > sizeof(mainBuffer)) {
            return OptStatus::BufferTooSmall;
        }
        memcpy(mainBuffer, request.mainVerbatim, request.mainVerbatimLen);
        mainLength = request.mainVerbatimLen;
    } else {
        CborWriter mainWriter(mainBuffer, sizeof(mainBuffer));
        const size_t fieldCount = encodeMain(request.main, mainWriter, true);
        mainWriter = CborWriter(mainBuffer, sizeof(mainBuffer));
        mainWriter.writeMapHeader(fieldCount);
        encodeMain(request.main, mainWriter, false);
        if (!mainWriter.ok()) {
            return OptStatus::BufferTooSmall;
        }
        mainLength = mainWriter.length();
    }

    // The aux region must start on a tag block boundary, and the meta section's
    // own length depends on the offset it encodes, so settle both together.
    uint8_t metaBuffer[8];
    size_t metaLength = 0;
    size_t auxOffset = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        metaLength = encodeMeta(static_cast<uint16_t>(auxOffset), true,
                                metaBuffer, sizeof(metaBuffer));
        if (metaLength == 0) {
            return OptStatus::BufferTooSmall;
        }
        const size_t unaligned = payloadStart + metaLength + mainLength;
        const size_t aligned = alignUp(unaligned, kBlockSize);
        const size_t candidate = aligned - payloadStart;
        if (candidate == auxOffset) {
            break;
        }
        auxOffset = candidate;
    }

    if (auxOffset + request.minAuxSize > payloadSize) {
        return OptStatus::BufferTooSmall;
    }
    const size_t auxSize = payloadSize - auxOffset;

    // Auxiliary seed: an indefinite-length map, as the specification
    // recommends, so other writers can extend it in place.
    uint8_t auxBuffer[64];
    CborWriter auxWriter(auxBuffer, sizeof(auxBuffer));
    auxWriter.writeIndefiniteMap();
    if (request.writeConsumedWeight) {
        auxWriter.writeUnsigned(aux_key::kConsumedWeight);
        auxWriter.writeNumber(request.consumedWeight);
    }
    if (request.workgroup[0] != '\0') {
        auxWriter.writeUnsigned(aux_key::kWorkgroup);
        auxWriter.writeText(request.workgroup);
    }
    if (request.storageLocation[0] != '\0') {
        auxWriter.writeUnsigned(aux_key::kStorageLocation);
        auxWriter.writeText(request.storageLocation);
    }
    auxWriter.writeBreak();
    if (!auxWriter.ok() || auxWriter.length() > auxSize) {
        return OptStatus::BufferTooSmall;
    }

    // Assemble the image.
    memset(out, 0, imageSize);
    size_t cursor = 0;
    out[cursor++] = kCcMagicE1;
    out[cursor++] = 0x40;  // mapping version 1.0, read and write allowed
    out[cursor++] = static_cast<uint8_t>(imageSize / 8);
    out[cursor++] = 0x01;  // supports multiple block reads

    const size_t messageLength = uriRecordLen + recordHeaderLen + payloadSize;
    out[cursor++] = kTlvNdef;
    if (bestShortTlv) {
        out[cursor++] = static_cast<uint8_t>(messageLength);
    } else {
        out[cursor++] = 0xFF;
        out[cursor++] = static_cast<uint8_t>(messageLength >> 8);
        out[cursor++] = static_cast<uint8_t>(messageLength);
    }

    if (uriRecordLen > 0) {
        out[cursor++] = 0x91;  // MB, SR, TNF=well known
        out[cursor++] = 0x01;
        out[cursor++] = static_cast<uint8_t>(uriPayloadLen);
        out[cursor++] = 'U';
        out[cursor++] = uriAbbreviation;
        const size_t restLen = uriPayloadLen - 1;
        memcpy(out + cursor, uriRest, restLen);
        cursor += restLen;
    }

    // Message end flag; message begin too when there is no URI record ahead.
    uint8_t flags = 0x40 | 0x02;  // ME, TNF=media type
    if (uriRecordLen == 0) {
        flags |= 0x80;  // MB
    }
    if (bestShortRecord) {
        flags |= 0x10;  // SR
    }
    out[cursor++] = flags;
    out[cursor++] = static_cast<uint8_t>(kMimeTypeLen);
    if (bestShortRecord) {
        out[cursor++] = static_cast<uint8_t>(payloadSize);
    } else {
        out[cursor++] = static_cast<uint8_t>(payloadSize >> 24);
        out[cursor++] = static_cast<uint8_t>(payloadSize >> 16);
        out[cursor++] = static_cast<uint8_t>(payloadSize >> 8);
        out[cursor++] = static_cast<uint8_t>(payloadSize);
    }
    memcpy(out + cursor, kMimeType, kMimeTypeLen);
    cursor += kMimeTypeLen;

    if (cursor != payloadStart) {
        return OptStatus::BufferTooSmall;  // layout arithmetic disagreed
    }

    memcpy(out + payloadStart, metaBuffer, metaLength);
    memcpy(out + payloadStart + metaLength, mainBuffer, mainLength);
    memcpy(out + payloadStart + auxOffset, auxBuffer, auxWriter.length());

    out[payloadStart + payloadSize] = kTlvTerminator;
    outLen = imageSize;

    if (regions != nullptr) {
        regions->ndefTlvStart = 4;
        regions->payloadStart = static_cast<uint16_t>(payloadStart);
        regions->payloadSize = static_cast<uint16_t>(payloadSize);
        regions->metaSize = static_cast<uint16_t>(metaLength);
        regions->mainOffset = static_cast<uint16_t>(metaLength);
        regions->mainSize = static_cast<uint16_t>(auxOffset - metaLength);
        regions->hasAux = true;
        regions->auxOffset = static_cast<uint16_t>(auxOffset);
        regions->auxSize = static_cast<uint16_t>(auxSize);
        regions->ccCapacity = static_cast<uint16_t>(imageSize);
    }

    return OptStatus::Ok;
}

OptStatus optBuildAuxRegion(const uint8_t *region, size_t regionLen,
                            const OptAuxUpdate &update, uint8_t *out,
                            size_t outCap, size_t &outLen) {
    outLen = 0;
    CborWriter writer(out, outCap);
    writer.writeIndefiniteMap();

    bool wroteConsumed = false;
    bool wroteWorkgroup = false;
    bool wroteStorage = false;

    // Rewrite existing entries in place. Keeping each field at its original
    // offset is what makes the block diff small: only the bytes that actually
    // changed move.
    CborMapIter iter(region, regionLen, 0);
    if (iter.valid()) {
        uint64_t key = 0;
        size_t valuePos = 0;
        size_t valueEnd = 0;
        while (iter.next(key, valuePos, valueEnd)) {
            if (key == aux_key::kConsumedWeight && update.setConsumedWeight) {
                writer.writeUnsigned(aux_key::kConsumedWeight);
                writer.writeNumber(update.consumedWeight);
                wroteConsumed = true;
                continue;
            }
            if (key == aux_key::kWorkgroup && update.setWorkgroup) {
                writer.writeUnsigned(aux_key::kWorkgroup);
                writer.writeText(update.workgroup);
                wroteWorkgroup = true;
                continue;
            }
            if (key == aux_key::kStorageLocation && update.setStorageLocation) {
                writer.writeUnsigned(aux_key::kStorageLocation);
                writer.writeText(update.storageLocation);
                wroteStorage = true;
                continue;
            }
            if (update.wipeUnknown && !isKnownAuxKey(key)) {
                continue;
            }

            // Unrecognised fields are preserved byte for byte, as the
            // specification requires of any update operation.
            writer.writeUnsigned(key);
            writer.writeRaw(region + valuePos, valueEnd - valuePos);
        }
    }

    if (update.setConsumedWeight && !wroteConsumed) {
        writer.writeUnsigned(aux_key::kConsumedWeight);
        writer.writeNumber(update.consumedWeight);
    }
    if (update.setWorkgroup && !wroteWorkgroup) {
        writer.writeUnsigned(aux_key::kWorkgroup);
        writer.writeText(update.workgroup);
    }
    if (update.setStorageLocation && !wroteStorage) {
        writer.writeUnsigned(aux_key::kStorageLocation);
        writer.writeText(update.storageLocation);
    }

    writer.writeBreak();
    if (!writer.ok()) {
        return OptStatus::BufferTooSmall;
    }
    outLen = writer.length();
    return OptStatus::Ok;
}

bool optDiffBlocks(const uint8_t *oldData, const uint8_t *newData, size_t len,
                   size_t blockSize, size_t &firstBlock, size_t &blockCount) {
    firstBlock = 0;
    blockCount = 0;
    if (oldData == nullptr || newData == nullptr || blockSize == 0) {
        return false;
    }

    size_t first = len;
    size_t last = 0;
    for (size_t i = 0; i < len; ++i) {
        if (oldData[i] != newData[i]) {
            if (first == len) {
                first = i;
            }
            last = i;
        }
    }
    if (first == len) {
        return false;
    }

    firstBlock = first / blockSize;
    const size_t lastBlock = last / blockSize;
    blockCount = lastBlock - firstBlock + 1;
    return true;
}

}  // namespace opt
