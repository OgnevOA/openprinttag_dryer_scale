// OpenPrintTag tag image codec.
//
// Handles the whole on-tag structure, not just the CBOR payload:
//
//   CC (4 B) | NDEF TLV | NDEF record(s) | TLV terminator
//                          `-- payload: meta | main region | aux region
//
// Reference: https://specs.openprinttag.org/#/nfc_data_format
//
// Plain C++ with no Arduino dependencies so the desktop harness in tests/ can
// exercise it against the specification's own fixtures.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace opt {

constexpr const char *kMimeType = "application/vnd.openprinttag";
constexpr size_t kMimeTypeLen = 28;

constexpr uint8_t kCcMagicE1 = 0xE1;  // 1-byte address CC
constexpr uint8_t kCcMagicE2 = 0xE2;  // 2-byte address CC
constexpr uint8_t kTlvNdef = 0x03;
constexpr uint8_t kTlvTerminator = 0xFE;
constexpr uint8_t kTlvLock = 0x01;
constexpr uint8_t kTlvMemory = 0x02;
constexpr uint8_t kTlvProprietary = 0xFD;

// Meta section keys.
namespace meta_key {
constexpr uint16_t kMainRegionOffset = 0;
constexpr uint16_t kMainRegionSize = 1;
constexpr uint16_t kAuxRegionOffset = 2;
constexpr uint16_t kAuxRegionSize = 3;
}  // namespace meta_key

// Main section keys (the subset this firmware understands).
namespace main_key {
constexpr uint16_t kInstanceUuid = 0;
constexpr uint16_t kPackageUuid = 1;
constexpr uint16_t kMaterialUuid = 2;
constexpr uint16_t kBrandUuid = 3;
constexpr uint16_t kGtin = 4;
constexpr uint16_t kBrandSpecificInstanceId = 5;
constexpr uint16_t kBrandSpecificPackageId = 6;
constexpr uint16_t kBrandSpecificMaterialId = 7;
constexpr uint16_t kMaterialClass = 8;
constexpr uint16_t kMaterialType = 9;
constexpr uint16_t kMaterialName = 10;
constexpr uint16_t kBrandName = 11;
constexpr uint16_t kWriteProtection = 13;
constexpr uint16_t kManufacturedDate = 14;
constexpr uint16_t kExpirationDate = 15;
constexpr uint16_t kNominalNettoFullWeight = 16;
constexpr uint16_t kActualNettoFullWeight = 17;
constexpr uint16_t kEmptyContainerWeight = 18;
constexpr uint16_t kPrimaryColor = 19;
constexpr uint16_t kTags = 28;
constexpr uint16_t kDensity = 29;
constexpr uint16_t kFilamentDiameter = 30;
constexpr uint16_t kMinPrintTemperature = 34;
constexpr uint16_t kMaxPrintTemperature = 35;
constexpr uint16_t kPreheatTemperature = 36;
constexpr uint16_t kMinBedTemperature = 37;
constexpr uint16_t kMaxBedTemperature = 38;
constexpr uint16_t kMinChamberTemperature = 39;
constexpr uint16_t kMaxChamberTemperature = 40;
constexpr uint16_t kChamberTemperature = 41;
constexpr uint16_t kContainerWidth = 42;
constexpr uint16_t kContainerOuterDiameter = 43;
constexpr uint16_t kContainerInnerDiameter = 44;
constexpr uint16_t kContainerHoleDiameter = 45;
constexpr uint16_t kMaterialAbbreviation = 52;
constexpr uint16_t kNominalFullLength = 53;
constexpr uint16_t kActualFullLength = 54;
constexpr uint16_t kDryingTemperature = 57;
constexpr uint16_t kDryingTime = 58;
}  // namespace main_key

// Auxiliary section keys.
namespace aux_key {
constexpr uint16_t kConsumedWeight = 0;
constexpr uint16_t kWorkgroup = 1;
constexpr uint16_t kGeneralPurposeRangeUser = 2;
constexpr uint16_t kLastStirTime = 3;
constexpr uint16_t kStorageLocation = 4;
constexpr uint16_t kPurchaseTime = 5;
constexpr uint16_t kPurchasePrice = 6;
constexpr uint16_t kPurchaseCurrency = 7;
}  // namespace aux_key

// Buffer sizes follow the max_length values in the specification's field
// definitions, plus one byte for the terminator.
constexpr size_t kMaterialNameSize = 64;
constexpr size_t kBrandNameSize = 32;
constexpr size_t kBrandIdSize = 17;
constexpr size_t kAbbreviationSize = 8;
constexpr size_t kWorkgroupSize = 9;
constexpr size_t kStorageLocationSize = 9;
constexpr size_t kCurrencySize = 4;
constexpr size_t kMaxMaterialTags = 8;
constexpr size_t kUuidSize = 16;
constexpr size_t kUriSize = 96;

// SLIX2 geometry. 316 B of user memory in 4-byte blocks.
constexpr size_t kBlockSize = 4;
constexpr size_t kSlix2Blocks = 79;
constexpr size_t kSlix2Bytes = kSlix2Blocks * kBlockSize;
constexpr size_t kMaxTagImage = 512;

enum class OptStatus : uint8_t {
    Ok = 0,
    Truncated,          // ran off the end of the supplied buffer
    NoCapabilityContainer,
    NoNdefTlv,
    NoOptRecord,        // no NDEF record with the OpenPrintTag MIME type
    BadMetaSection,
    BadMainSection,
    BadAuxSection,
    RegionOutOfRange,
    BufferTooSmall,
};

const char *optStatusText(OptStatus status);

struct OptRegions {
    uint16_t ndefTlvStart = 0;    // offset of the 0x03 TLV tag
    uint16_t payloadStart = 0;    // absolute offset of the record payload
    uint16_t payloadSize = 0;
    uint16_t metaSize = 0;        // bytes the meta CBOR object occupies

    uint16_t mainOffset = 0;      // relative to payloadStart
    uint16_t mainSize = 0;
    bool hasAux = false;
    uint16_t auxOffset = 0;
    uint16_t auxSize = 0;

    uint16_t ccCapacity = 0;      // MLEN * 8 from the capability container

    uint16_t mainAbsolute() const {
        return static_cast<uint16_t>(payloadStart + mainOffset);
    }
    uint16_t auxAbsolute() const {
        return static_cast<uint16_t>(payloadStart + auxOffset);
    }
};

struct OptMainData {
    bool hasMaterialClass = false;
    uint16_t materialClass = 0;
    bool hasMaterialType = false;
    uint16_t materialType = 0;

    char materialName[kMaterialNameSize] = {0};
    char materialAbbreviation[kAbbreviationSize] = {0};
    char brandName[kBrandNameSize] = {0};
    char brandSpecificInstanceId[kBrandIdSize] = {0};

    // Identity. Nothing here is used to make a decision, but all of it is
    // carried so that re-encoding a tag another tool wrote does not throw away
    // the only fields that tie a spool to a product and a brand. Deriving them
    // needs a UUIDv5, so a lost one cannot be reconstructed on the device.
    bool hasInstanceUuid = false;
    uint8_t instanceUuid[kUuidSize] = {0};
    bool hasPackageUuid = false;
    uint8_t packageUuid[kUuidSize] = {0};
    bool hasMaterialUuid = false;
    uint8_t materialUuid[kUuidSize] = {0};
    bool hasBrandUuid = false;
    uint8_t brandUuid[kUuidSize] = {0};

    bool hasGtin = false;
    uint64_t gtin = 0;

    bool hasManufacturedDate = false;
    uint32_t manufacturedDate = 0;
    bool hasExpirationDate = false;
    uint32_t expirationDate = 0;

    bool hasNominalNettoFullWeight = false;
    float nominalNettoFullWeight = 0.0f;
    bool hasActualNettoFullWeight = false;
    float actualNettoFullWeight = 0.0f;
    bool hasEmptyContainerWeight = false;
    float emptyContainerWeight = 0.0f;

    bool hasNominalFullLength = false;
    float nominalFullLength = 0.0f;   // mm
    bool hasActualFullLength = false;
    float actualFullLength = 0.0f;    // mm

    bool hasPrimaryColor = false;
    uint8_t primaryColor[4] = {0, 0, 0, 0xFF};
    uint8_t primaryColorLen = 0;

    bool hasDensity = false;
    float density = 0.0f;             // g/cm3
    bool hasFilamentDiameter = false;
    float filamentDiameter = 0.0f;    // mm

    bool hasMinPrintTemperature = false;
    int16_t minPrintTemperature = 0;
    bool hasMaxPrintTemperature = false;
    int16_t maxPrintTemperature = 0;
    bool hasPreheatTemperature = false;
    int16_t preheatTemperature = 0;
    bool hasMinBedTemperature = false;
    int16_t minBedTemperature = 0;
    bool hasMaxBedTemperature = false;
    int16_t maxBedTemperature = 0;
    bool hasMinChamberTemperature = false;
    int16_t minChamberTemperature = 0;
    bool hasMaxChamberTemperature = false;
    int16_t maxChamberTemperature = 0;
    bool hasChamberTemperature = false;
    int16_t chamberTemperature = 0;
    bool hasDryingTemperature = false;
    int16_t dryingTemperature = 0;
    bool hasDryingTime = false;
    int16_t dryingTime = 0;           // minutes

    bool hasContainerWidth = false;
    int16_t containerWidth = 0;
    bool hasContainerOuterDiameter = false;
    int16_t containerOuterDiameter = 0;
    bool hasContainerInnerDiameter = false;
    int16_t containerInnerDiameter = 0;
    bool hasContainerHoleDiameter = false;
    int16_t containerHoleDiameter = 0;

    bool hasWriteProtection = false;
    uint16_t writeProtection = 0;

    uint16_t tags[kMaxMaterialTags] = {0};
    uint8_t tagCount = 0;
    bool tagsTruncated = false;

    uint8_t unknownFieldCount = 0;    // diagnostics only
};

struct OptAuxData {
    bool hasConsumedWeight = false;
    float consumedWeight = 0.0f;      // g
    char workgroup[kWorkgroupSize] = {0};
    char storageLocation[kStorageLocationSize] = {0};
    bool hasPurchaseTime = false;
    uint32_t purchaseTime = 0;
    bool hasPurchasePrice = false;
    float purchasePrice = 0.0f;
    char purchaseCurrency[kCurrencySize] = {0};
    uint8_t unknownFieldCount = 0;
};

struct OptTag {
    OptRegions regions;
    OptMainData main;
    OptAuxData aux;
    bool hasUriRecord = false;

    // The URI record's text, with NDEF's one-byte prefix abbreviation expanded
    // back out. It is kept because the record sits outside the OpenPrintTag
    // payload, so nothing else here describes it, and a rebuild that did not
    // know the text would silently drop a link the manufacturer put on the tag.
    //
    // Empty while `hasUriRecord` is set means the record is there but this
    // decoder cannot reproduce it: longer than the buffer, or a prefix
    // abbreviation `optBuildImage` does not implement. A caller rebuilding the
    // tag has to treat that as a refusal rather than writing a different URI.
    char uri[kUriSize] = {0};

    // A malformed auxiliary region does not invalidate the tag: the static main
    // data is still trustworthy, and the aux region is exactly the part any
    // other device is allowed to have scribbled on. Callers check this to
    // decide whether to offer a wipe.
    OptStatus auxStatus = OptStatus::Ok;
};

// Parses a complete tag image. `len` is how much of the tag was read; parsing a
// short read fails cleanly rather than reading past the end.
OptStatus optParse(const uint8_t *image, size_t len, OptTag &out);

// Parses just a region's CBOR map. Used after a targeted re-read of the aux
// region, so the whole image does not have to be held in memory.
OptStatus optParseMain(const uint8_t *region, size_t len, OptMainData &out);
OptStatus optParseAux(const uint8_t *region, size_t len, OptAuxData &out);

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

// What to put on a freshly initialised tag. Only fields with their `has` flag
// set (or a non-empty string) are written, so a caller can populate as much or
// as little as it knows.
struct OptBuildRequest {
    OptMainData main;

    // Auxiliary region seed.
    char workgroup[kWorkgroupSize] = {0};
    char storageLocation[kStorageLocationSize] = {0};
    float consumedWeight = 0.0f;
    bool writeConsumedWeight = true;

    // Usable bytes on the tag, including the capability container: 316 for a
    // SLIX2. The image is rounded down to a multiple of 8 because the CC
    // expresses its length in 8-byte units.
    size_t tagCapacity = kSlix2Bytes;

    // Minimum bytes to reserve for the auxiliary region. The region is placed
    // last and given every remaining byte, so this is a floor, not a cap.
    size_t minAuxSize = 32;

    // Optional leading NDEF URI record, off by default: it competes with the
    // payload for the tag's scarce memory.
    const char *uri = nullptr;

    // A main region lifted byte for byte off a tag that is being rebuilt, used
    // in place of encoding `main`. This is the only way to keep fields the
    // firmware does not model -- certifications, secondary colours, an
    // explicitly stored UUID -- through a change of layout, since anything the
    // decoder cannot represent, the encoder cannot reproduce. Must point
    // outside the output buffer and outlive the call.
    const uint8_t *mainVerbatim = nullptr;
    size_t mainVerbatimLen = 0;
};

// Finds the main region's CBOR map on a parsed tag, excluding any padding that
// follows it, so it can be handed back through `mainVerbatim`. Fails if the
// region does not hold a map.
bool optMainRegionSpan(const uint8_t *image, size_t len, const OptTag &tag,
                       const uint8_t *&start, size_t &length);

// Builds a complete tag image (CC through TLV terminator). Bytes past the
// written image are zero filled up to the tag capacity, per the specification's
// recommendation to zero unused space on initialisation.
OptStatus optBuildImage(const OptBuildRequest &request, uint8_t *out,
                        size_t outCap, size_t &outLen, OptRegions *regions);

// Fields an aux update may change. Unset members are left as they are on the
// tag, and unknown keys are always carried over verbatim.
struct OptAuxUpdate {
    bool setConsumedWeight = false;
    float consumedWeight = 0.0f;
    bool setWorkgroup = false;
    char workgroup[kWorkgroupSize] = {0};
    bool setStorageLocation = false;
    char storageLocation[kStorageLocationSize] = {0};

    // Drops every field the firmware does not recognise, for the workgroup
    // mismatch case where the existing aux content is not to be trusted.
    bool wipeUnknown = false;
};

// Re-encodes an auxiliary region. `region`/`regionLen` is the current content
// (may be all zeros on a blank region); the result is written to `out`.
OptStatus optBuildAuxRegion(const uint8_t *region, size_t regionLen,
                            const OptAuxUpdate &update, uint8_t *out,
                            size_t outCap, size_t &outLen);

// Finds the smallest block-aligned span covering the differences between two
// buffers, so an update writes as few blocks as possible. Returns false when
// the buffers already match.
bool optDiffBlocks(const uint8_t *oldData, const uint8_t *newData, size_t len,
                   size_t blockSize, size_t &firstBlock, size_t &blockCount);

}  // namespace opt
