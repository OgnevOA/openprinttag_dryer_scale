// Desktop tests for the OpenPrintTag codec.
//
// Two kinds of checks:
//
//  1. Decoding the binary fixtures shipped with the specification and comparing
//     the result against the reference implementation's own decode of the same
//     bytes (see tools/gen_fixture_expectations.py).
//  2. Round-tripping our encoder through our decoder, including the layout
//     arithmetic for a 316-byte SLIX2 and the auxiliary-region update rules.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../firmware/OpenPrintTagDryer/src/openprinttag/opt_cbor.h"
#include "../firmware/OpenPrintTagDryer/src/openprinttag/opt_enums.h"
#include "../firmware/OpenPrintTagDryer/src/openprinttag/opt_tag.h"
#include "fixture_expectations.h"

namespace {

int gChecks = 0;
int gFailures = 0;
const char *gCurrentTest = "";

void fail(const char *what, const std::string &detail) {
    ++gFailures;
    printf("  FAIL %s: %s\n        %s\n", gCurrentTest, what, detail.c_str());
}

void check(bool condition, const char *what, const std::string &detail = "") {
    ++gChecks;
    if (!condition) {
        fail(what, detail);
    }
}

void checkEqualInt(long long actual, long long expected, const char *what) {
    ++gChecks;
    if (actual != expected) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "expected %lld, got %lld", expected,
                 actual);
        fail(what, buffer);
    }
}

void checkEqualFloat(double actual, double expected, const char *what,
                     double tolerance = 1e-3) {
    ++gChecks;
    const double allowed = fabs(expected) * tolerance;
    if (fabs(actual - expected) > (allowed > 1e-6 ? allowed : 1e-6)) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "expected %g, got %g", expected,
                 actual);
        fail(what, buffer);
    }
}

void checkEqualText(const char *actual, const char *expected,
                    const char *what) {
    ++gChecks;
    if (strcmp(actual, expected) != 0) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "expected \"%s\", got \"%s\"",
                 expected, actual);
        fail(what, buffer);
    }
}

std::string toHex(const uint8_t *data, size_t len) {
    static const char *digits = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result.push_back(digits[data[i] >> 4]);
        result.push_back(digits[data[i] & 0x0F]);
    }
    return result;
}

std::vector<uint8_t> fromHex(const std::string &hex) {
    std::vector<uint8_t> result;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        result.push_back(static_cast<uint8_t>(
            std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return result;
}

bool readFile(const char *path, std::vector<uint8_t> &out) {
    FILE *handle = fopen(path, "rb");
    if (handle == nullptr) {
        return false;
    }
    out.clear();
    uint8_t buffer[512];
    size_t read = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), handle)) > 0) {
        out.insert(out.end(), buffer, buffer + read);
    }
    fclose(handle);
    return true;
}

// ---------------------------------------------------------------------------
// Field accessors, mirroring the generated expectation table back onto the
// decoded structs.
// ---------------------------------------------------------------------------

bool mainInt(const opt::OptMainData &main, uint16_t key, long long &out) {
    switch (key) {
    case opt::main_key::kGtin:
        if (!main.hasGtin) return false;
        out = static_cast<long long>(main.gtin);
        return true;
    case opt::main_key::kManufacturedDate:
        if (!main.hasManufacturedDate) return false;
        out = main.manufacturedDate;
        return true;
    case opt::main_key::kExpirationDate:
        if (!main.hasExpirationDate) return false;
        out = main.expirationDate;
        return true;
    case opt::main_key::kMaterialClass:
        if (!main.hasMaterialClass) return false;
        out = main.materialClass;
        return true;
    case opt::main_key::kMaterialType:
        if (!main.hasMaterialType) return false;
        out = main.materialType;
        return true;
    case opt::main_key::kWriteProtection:
        if (!main.hasWriteProtection) return false;
        out = main.writeProtection;
        return true;
    case opt::main_key::kMinPrintTemperature:
        if (!main.hasMinPrintTemperature) return false;
        out = main.minPrintTemperature;
        return true;
    case opt::main_key::kMaxPrintTemperature:
        if (!main.hasMaxPrintTemperature) return false;
        out = main.maxPrintTemperature;
        return true;
    case opt::main_key::kPreheatTemperature:
        if (!main.hasPreheatTemperature) return false;
        out = main.preheatTemperature;
        return true;
    case opt::main_key::kMinBedTemperature:
        if (!main.hasMinBedTemperature) return false;
        out = main.minBedTemperature;
        return true;
    case opt::main_key::kMaxBedTemperature:
        if (!main.hasMaxBedTemperature) return false;
        out = main.maxBedTemperature;
        return true;
    case opt::main_key::kMinChamberTemperature:
        if (!main.hasMinChamberTemperature) return false;
        out = main.minChamberTemperature;
        return true;
    case opt::main_key::kMaxChamberTemperature:
        if (!main.hasMaxChamberTemperature) return false;
        out = main.maxChamberTemperature;
        return true;
    case opt::main_key::kChamberTemperature:
        if (!main.hasChamberTemperature) return false;
        out = main.chamberTemperature;
        return true;
    case opt::main_key::kDryingTemperature:
        if (!main.hasDryingTemperature) return false;
        out = main.dryingTemperature;
        return true;
    case opt::main_key::kDryingTime:
        if (!main.hasDryingTime) return false;
        out = main.dryingTime;
        return true;
    case opt::main_key::kContainerWidth:
        if (!main.hasContainerWidth) return false;
        out = main.containerWidth;
        return true;
    case opt::main_key::kContainerOuterDiameter:
        if (!main.hasContainerOuterDiameter) return false;
        out = main.containerOuterDiameter;
        return true;
    case opt::main_key::kContainerInnerDiameter:
        if (!main.hasContainerInnerDiameter) return false;
        out = main.containerInnerDiameter;
        return true;
    case opt::main_key::kContainerHoleDiameter:
        if (!main.hasContainerHoleDiameter) return false;
        out = main.containerHoleDiameter;
        return true;
    default:
        return false;
    }
}

bool mainFloat(const opt::OptMainData &main, uint16_t key, double &out) {
    switch (key) {
    case opt::main_key::kNominalNettoFullWeight:
        if (!main.hasNominalNettoFullWeight) return false;
        out = main.nominalNettoFullWeight;
        return true;
    case opt::main_key::kActualNettoFullWeight:
        if (!main.hasActualNettoFullWeight) return false;
        out = main.actualNettoFullWeight;
        return true;
    case opt::main_key::kEmptyContainerWeight:
        if (!main.hasEmptyContainerWeight) return false;
        out = main.emptyContainerWeight;
        return true;
    case opt::main_key::kNominalFullLength:
        if (!main.hasNominalFullLength) return false;
        out = main.nominalFullLength;
        return true;
    case opt::main_key::kActualFullLength:
        if (!main.hasActualFullLength) return false;
        out = main.actualFullLength;
        return true;
    case opt::main_key::kDensity:
        if (!main.hasDensity) return false;
        out = main.density;
        return true;
    case opt::main_key::kFilamentDiameter:
        if (!main.hasFilamentDiameter) return false;
        out = main.filamentDiameter;
        return true;
    default:
        return false;
    }
}

const char *mainText(const opt::OptMainData &main, uint16_t key) {
    switch (key) {
    case opt::main_key::kMaterialName: return main.materialName;
    case opt::main_key::kMaterialAbbreviation: return main.materialAbbreviation;
    case opt::main_key::kBrandName: return main.brandName;
    case opt::main_key::kBrandSpecificInstanceId:
        return main.brandSpecificInstanceId;
    default: return nullptr;
    }
}

const char *auxText(const opt::OptAuxData &aux, uint16_t key) {
    switch (key) {
    case opt::aux_key::kWorkgroup: return aux.workgroup;
    case opt::aux_key::kStorageLocation: return aux.storageLocation;
    case opt::aux_key::kPurchaseCurrency: return aux.purchaseCurrency;
    default: return nullptr;
    }
}

void checkMainField(const opt::OptMainData &main, const fixtures::Field &field) {
    char label[160];
    snprintf(label, sizeof(label), "main.%s", field.name);

    switch (field.kind) {
    case fixtures::Kind::Int:
    case fixtures::Kind::Enum: {
        long long value = 0;
        if (!mainInt(main, field.key, value)) {
            check(false, label, "field missing from decode");
            return;
        }
        checkEqualInt(value, field.integer, label);
        return;
    }
    case fixtures::Kind::Float: {
        double value = 0.0;
        if (!mainFloat(main, field.key, value)) {
            check(false, label, "field missing from decode");
            return;
        }
        checkEqualFloat(value, field.floating, label);
        return;
    }
    case fixtures::Kind::Text: {
        const char *value = mainText(main, field.key);
        if (value == nullptr) {
            check(false, label, "no accessor for key");
            return;
        }
        checkEqualText(value, field.text, label);
        return;
    }
    case fixtures::Kind::Color: {
        const std::vector<uint8_t> expected = fromHex(field.text);
        check(main.hasPrimaryColor, label, "colour missing from decode");
        checkEqualInt(main.primaryColorLen,
                      static_cast<long long>(expected.size()),
                      "main.primary_color length");
        for (size_t i = 0; i < expected.size() && i < 4; ++i) {
            checkEqualInt(main.primaryColor[i], expected[i], label);
        }
        return;
    }
    case fixtures::Kind::Uuid: {
        const std::vector<uint8_t> expected = fromHex(field.text);
        const uint8_t *value = nullptr;
        bool present = false;
        switch (field.key) {
        case opt::main_key::kInstanceUuid:
            value = main.instanceUuid;
            present = main.hasInstanceUuid;
            break;
        case opt::main_key::kPackageUuid:
            value = main.packageUuid;
            present = main.hasPackageUuid;
            break;
        case opt::main_key::kMaterialUuid:
            value = main.materialUuid;
            present = main.hasMaterialUuid;
            break;
        case opt::main_key::kBrandUuid:
            value = main.brandUuid;
            present = main.hasBrandUuid;
            break;
        default:
            check(false, label, "no accessor for key");
            return;
        }
        check(present, label, "uuid missing from decode");
        checkEqualInt(expected.size(), opt::kUuidSize, "uuid length in fixture");
        for (size_t i = 0; i < expected.size() && i < opt::kUuidSize; ++i) {
            checkEqualInt(value[i], expected[i], label);
        }
        return;
    }
    case fixtures::Kind::TagList: {
        std::vector<long long> expected;
        const std::string list = field.text;
        size_t start = 0;
        while (start < list.size()) {
            const size_t comma = list.find(',', start);
            const std::string piece = list.substr(
                start, comma == std::string::npos ? std::string::npos
                                                  : comma - start);
            if (!piece.empty()) {
                expected.push_back(std::stoll(piece));
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        checkEqualInt(main.tagCount, static_cast<long long>(expected.size()),
                      "main.tags count");
        for (size_t i = 0; i < expected.size() && i < main.tagCount; ++i) {
            checkEqualInt(main.tags[i], expected[i], label);
        }
        return;
    }
    }
}

void checkAuxField(const opt::OptAuxData &aux, const fixtures::Field &field) {
    char label[160];
    snprintf(label, sizeof(label), "aux.%s", field.name);

    switch (field.kind) {
    case fixtures::Kind::Float:
        if (field.key == opt::aux_key::kConsumedWeight) {
            check(aux.hasConsumedWeight, label, "consumed weight missing");
            checkEqualFloat(aux.consumedWeight, field.floating, label);
        } else if (field.key == opt::aux_key::kPurchasePrice) {
            check(aux.hasPurchasePrice, label, "purchase price missing");
            checkEqualFloat(aux.purchasePrice, field.floating, label);
        }
        return;
    case fixtures::Kind::Int:
        if (field.key == opt::aux_key::kPurchaseTime) {
            check(aux.hasPurchaseTime, label, "purchase time missing");
            checkEqualInt(aux.purchaseTime, field.integer, label);
        }
        return;
    case fixtures::Kind::Text: {
        const char *value = auxText(aux, field.key);
        if (value == nullptr) {
            check(false, label, "no accessor for key");
            return;
        }
        checkEqualText(value, field.text, label);
        return;
    }
    default:
        return;
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testCborPrimitives() {
    gCurrentTest = "cbor primitives";

    // Half floats the specification's own fixtures rely on.
    checkEqualFloat(opt::halfToFloat(0x3CF6), 1.240234375, "half 0x3cf6");
    checkEqualFloat(opt::halfToFloat(0x3F00), 1.75, "half 0x3f00");
    checkEqualFloat(opt::halfToFloat(0x3E00), 1.5, "half 0x3e00");
    checkEqualFloat(opt::halfToFloat(0x0000), 0.0, "half zero");

    uint16_t half = 0;
    check(opt::floatToHalf(1.75, half), "1.75 fits a half float");
    checkEqualInt(half, 0x3F00, "1.75 encodes as 0x3f00");
    check(opt::floatToHalf(1.24, half), "1.24 within tolerance as half");
    check(!opt::floatToHalf(1.0e30, half), "1e30 rejected as half float");

    // Writer and reader agree, including the integer-preferred number path.
    uint8_t buffer[64];
    opt::CborWriter writer(buffer, sizeof(buffer));
    writer.writeMapHeader(3);
    writer.writeUnsigned(1);
    writer.writeNumber(1013.0);
    writer.writeUnsigned(2);
    writer.writeText("myhome");
    writer.writeUnsigned(3);
    writer.writeNumber(1.75);
    check(writer.ok(), "writer stayed within capacity");

    opt::CborMapIter iter(buffer, writer.length(), 0);
    check(iter.valid(), "encoded map parses");
    uint64_t key = 0;
    size_t valuePos = 0;
    size_t valueEnd = 0;

    check(iter.next(key, valuePos, valueEnd), "first entry");
    checkEqualInt(key, 1, "first key");
    uint64_t whole = 0;
    check(opt::cborUnsigned(buffer, writer.length(), valuePos, whole),
          "1013 decoded as an integer, not a float");
    checkEqualInt(whole, 1013, "1013 round trip");

    check(iter.next(key, valuePos, valueEnd), "second entry");
    char text[16] = {0};
    check(opt::cborText(buffer, writer.length(), valuePos, text, sizeof(text)),
          "text decodes");
    checkEqualText(text, "myhome", "text round trip");

    check(iter.next(key, valuePos, valueEnd), "third entry");
    double number = 0.0;
    check(opt::cborNumber(buffer, writer.length(), valuePos, number),
          "half float decodes");
    checkEqualFloat(number, 1.75, "1.75 round trip");

    check(!iter.next(key, valuePos, valueEnd), "map ends after three entries");

    // Overflow is sticky rather than silently truncating.
    uint8_t tiny[2];
    opt::CborWriter small(tiny, sizeof(tiny));
    small.writeText("far too long for two bytes");
    check(!small.ok(), "overflow flagged");
}

void testMalformedInput() {
    gCurrentTest = "malformed input";

    // Truncated head: a length promising more bytes than exist.
    const uint8_t truncatedText[] = {0x78, 0x40, 'a', 'b'};
    char text[8] = {0};
    check(!opt::cborText(truncatedText, sizeof(truncatedText), 0, text,
                         sizeof(text)),
          "truncated text string rejected");

    // A map claiming more entries than the buffer can hold must not spin.
    const uint8_t lyingMap[] = {0xBB, 0xFF, 0xFF, 0xFF, 0xFF,
                                0xFF, 0xFF, 0xFF, 0xFF};
    size_t next = 0;
    check(!opt::cborSkip(lyingMap, sizeof(lyingMap), 0, next),
          "oversized map length rejected");

    // Nesting deeper than the depth limit is refused rather than overflowing
    // the stack.
    std::vector<uint8_t> deep;
    for (int i = 0; i < 40; ++i) {
        deep.push_back(0x81);  // array(1)
    }
    deep.push_back(0x00);
    check(!opt::cborSkip(deep.data(), deep.size(), 0, next),
          "excessive nesting rejected");

    // Reserved additional-info values.
    const uint8_t reserved[] = {0x1C};
    opt::CborItem item;
    check(!opt::cborRead(reserved, sizeof(reserved), 0, item),
          "reserved additional info rejected");

    // A tag image that is not one at all.
    const uint8_t garbage[16] = {0};
    opt::OptTag tag;
    check(opt::optParse(garbage, sizeof(garbage), tag) !=
              opt::OptStatus::Ok,
          "zeroed image rejected");
}

void testFixtures() {
    for (size_t index = 0; index < fixtures::kFixtureCount; ++index) {
        const fixtures::Fixture &fixture = fixtures::kFixtures[index];
        char testName[64];
        snprintf(testName, sizeof(testName), "fixture %s", fixture.name);
        gCurrentTest = testName;

        std::vector<uint8_t> image;
        if (!readFile(fixture.path, image)) {
            check(false, "fixture readable", fixture.path);
            continue;
        }
        checkEqualInt(static_cast<long long>(image.size()), fixture.dataSize,
                      "image size");

        opt::OptTag tag;
        const opt::OptStatus status =
            opt::optParse(image.data(), image.size(), tag);
        check(status == opt::OptStatus::Ok, "parses",
              opt::optStatusText(status));
        if (status != opt::OptStatus::Ok) {
            continue;
        }

        if (fixture.ccCapacity != 0) {
            checkEqualInt(tag.regions.ccCapacity, fixture.ccCapacity,
                          "cc capacity");
        }
        checkEqualInt(tag.regions.payloadStart, fixture.payloadStart,
                      "payload start");
        checkEqualInt(tag.regions.payloadSize, fixture.payloadSize,
                      "payload size");
        checkEqualInt(tag.regions.metaSize, fixture.meta.size, "meta size");
        checkEqualInt(tag.regions.mainOffset, fixture.main.payloadOffset,
                      "main offset");
        checkEqualInt(tag.regions.mainSize, fixture.main.size, "main size");
        checkEqualInt(tag.regions.mainAbsolute(), fixture.main.absoluteOffset,
                      "main absolute offset");
        check(tag.regions.hasAux == fixture.hasAux, "aux presence");
        if (fixture.hasAux) {
            checkEqualInt(tag.regions.auxOffset, fixture.aux.payloadOffset,
                          "aux offset");
            checkEqualInt(tag.regions.auxSize, fixture.aux.size, "aux size");
            checkEqualInt(tag.regions.auxAbsolute(), fixture.aux.absoluteOffset,
                          "aux absolute offset");
            check(tag.regions.auxAbsolute() % opt::kBlockSize == 0,
                  "aux region is block aligned");
        }
        check(tag.hasUriRecord == fixture.hasUri, "uri record presence");

        // Byte-level agreement on region boundaries: the region contents we
        // isolate must equal the reference implementation's own dump.
        const std::string mainHex =
            toHex(image.data() + tag.regions.mainAbsolute(),
                  tag.regions.mainSize);
        check(mainHex == fixture.mainHex, "main region bytes");
        if (fixture.hasAux) {
            const std::string auxHex =
                toHex(image.data() + tag.regions.auxAbsolute(),
                      tag.regions.auxSize);
            check(auxHex == fixture.auxHex, "aux region bytes");
        }
        const std::string metaHex =
            toHex(image.data() + tag.regions.payloadStart,
                  tag.regions.metaSize);
        check(metaHex == fixture.metaHex, "meta region bytes");

        for (size_t i = 0; i < fixture.mainFieldCount; ++i) {
            checkMainField(tag.main, fixture.mainFields[i]);
        }
        for (size_t i = 0; i < fixture.auxFieldCount; ++i) {
            checkAuxField(tag.aux, fixture.auxFields[i]);
        }

        // Every field in the region is accounted for: either decoded into the
        // struct or counted as unknown. A mismatch means a field the firmware
        // should understand is being silently skipped.
        checkEqualInt(tag.main.unknownFieldCount, fixture.unmappedMainFields,
                      "unmapped main field count");
        if (fixture.hasAux && tag.auxStatus == opt::OptStatus::Ok) {
            checkEqualInt(tag.aux.unknownFieldCount, fixture.unmappedAuxFields,
                          "unmapped aux field count");
        }

        // A short read must fail cleanly at every truncation point.
        for (size_t length = 1; length < image.size(); length += 7) {
            opt::OptTag partial;
            opt::optParse(image.data(), length, partial);
        }
    }
}

void testSampleTag() {
    gCurrentTest = "sample_tag.bin";

    std::vector<uint8_t> image;
    if (!readFile("fixtures/sample_tag.bin", image)) {
        check(false, "sample tag readable");
        return;
    }

    opt::OptTag tag;
    const opt::OptStatus status = opt::optParse(image.data(), image.size(), tag);
    check(status == opt::OptStatus::Ok, "parses", opt::optStatusText(status));
    if (status != opt::OptStatus::Ok) {
        return;
    }
    checkEqualText(tag.main.brandName, "Prusament", "brand name");
    checkEqualText(tag.main.materialName, "PLA Galaxy Black", "material name");
    check(tag.main.hasMaterialType, "material type present");
    checkEqualText(opt::materialTypeName(tag.main.materialType), "PLA",
                   "material type name");
}

opt::OptMainData prusamentPla() {
    opt::OptMainData main;
    main.hasMaterialClass = true;
    main.materialClass = 0;  // FFF
    main.hasMaterialType = true;
    main.materialType = 0;   // PLA
    snprintf(main.materialName, sizeof(main.materialName),
             "PLA Prusa Galaxy Black");
    snprintf(main.brandName, sizeof(main.brandName), "Prusament");
    snprintf(main.brandSpecificInstanceId,
             sizeof(main.brandSpecificInstanceId), "334c54f088");
    main.hasNominalNettoFullWeight = true;
    main.nominalNettoFullWeight = 1000.0f;
    main.hasActualNettoFullWeight = true;
    main.actualNettoFullWeight = 1044.0f;
    main.hasEmptyContainerWeight = true;
    main.emptyContainerWeight = 201.0f;
    main.hasActualFullLength = true;
    main.actualFullLength = 343000.0f;
    main.hasDensity = true;
    main.density = 1.24f;
    main.hasFilamentDiameter = true;
    main.filamentDiameter = 1.75f;
    main.hasMinPrintTemperature = true;
    main.minPrintTemperature = 205;
    main.hasMaxPrintTemperature = true;
    main.maxPrintTemperature = 225;
    main.hasMinBedTemperature = true;
    main.minBedTemperature = 50;
    main.hasMaxBedTemperature = true;
    main.maxBedTemperature = 60;
    main.hasDryingTemperature = true;
    main.dryingTemperature = 55;
    main.hasDryingTime = true;
    main.dryingTime = 420;
    main.hasPrimaryColor = true;
    main.primaryColor[0] = 0x3D;
    main.primaryColor[1] = 0x3E;
    main.primaryColor[2] = 0x3D;
    main.primaryColor[3] = 0xFF;
    main.primaryColorLen = 4;
    main.hasWriteProtection = true;
    main.writeProtection = 0;  // not protected: our own tag
    main.tagCount = 1;
    main.tags[0] = 23;  // glitter
    return main;
}

// Fields the firmware has no use for but must not destroy. The specification
// allows a writer to store the identity UUIDs explicitly instead of leaving them
// to be derived, and the chamber temperatures are simply advice this device does
// not act on. Both only matter when an existing tag is rewritten: whatever the
// decoder fails to carry, the encoder silently drops.
void testCarriedFields() {
    gCurrentTest = "carried fields";

    opt::OptBuildRequest request;
    request.main = prusamentPla();
    request.main.hasInstanceUuid = true;
    for (size_t i = 0; i < opt::kUuidSize; ++i) {
        request.main.instanceUuid[i] = static_cast<uint8_t>(0xA0 + i);
    }
    request.main.hasBrandUuid = true;
    static const uint8_t kPrusamentBrandUuid[opt::kUuidSize] = {
        0xAE, 0x5F, 0xF3, 0x4E, 0x29, 0x8E, 0x50, 0xC9,
        0x8F, 0x77, 0x92, 0xA9, 0x7F, 0xB3, 0x0B, 0x09};
    memcpy(request.main.brandUuid, kPrusamentBrandUuid, opt::kUuidSize);
    request.main.hasMinChamberTemperature = true;
    request.main.minChamberTemperature = 18;
    request.main.hasMaxChamberTemperature = true;
    request.main.maxChamberTemperature = 40;
    request.main.hasChamberTemperature = true;
    request.main.chamberTemperature = 20;
    request.tagCapacity = opt::kSlix2Bytes;
    request.minAuxSize = 32;

    uint8_t image[opt::kMaxTagImage];
    size_t imageLen = 0;
    const opt::OptStatus status =
        opt::optBuildImage(request, image, sizeof(image), imageLen, nullptr);
    check(status == opt::OptStatus::Ok,
          "two explicit uuids and the chamber advice still fit a SLIX2",
          opt::optStatusText(status));
    if (status != opt::OptStatus::Ok) {
        return;
    }

    opt::OptTag tag;
    check(opt::optParse(image, imageLen, tag) == opt::OptStatus::Ok,
          "re-parses");
    check(tag.main.hasInstanceUuid, "instance uuid survives a rewrite");
    check(memcmp(tag.main.instanceUuid, request.main.instanceUuid,
                 opt::kUuidSize) == 0,
          "instance uuid byte for byte");
    check(tag.main.hasBrandUuid, "brand uuid survives a rewrite");
    check(memcmp(tag.main.brandUuid, kPrusamentBrandUuid, opt::kUuidSize) == 0,
          "brand uuid byte for byte");
    check(!tag.main.hasPackageUuid, "absent uuid stays absent");
    check(!tag.main.hasMaterialUuid, "absent uuid stays absent");
    checkEqualInt(tag.main.minChamberTemperature, 18, "chamber minimum");
    checkEqualInt(tag.main.maxChamberTemperature, 40, "chamber maximum");
    checkEqualInt(tag.main.chamberTemperature, 20, "ideal chamber temperature");
    checkEqualInt(tag.main.unknownFieldCount, 0,
                  "nothing landed in the unrecognised bucket");

    // A truncated UUID is not a UUID. Taking half of one would be worse than
    // ignoring it, because the next rewrite would present the fragment as fact.
    uint8_t region[64];
    size_t used = 0;
    region[used++] = 0xA1;        // map of one
    region[used++] = 0x00;        // key 0, instance_uuid
    region[used++] = 0x44;        // byte string of four
    for (int i = 0; i < 4; ++i) {
        region[used++] = 0x11;
    }
    opt::OptMainData shortUuid;
    check(opt::optParseMain(region, used, shortUuid) == opt::OptStatus::Ok,
          "region with a short uuid still parses");
    check(!shortUuid.hasInstanceUuid, "four-byte uuid rejected");
    checkEqualInt(shortUuid.unknownFieldCount, 1,
                  "and counted as unrecognised instead");
}

// Rebuilding a tag around the main region it already carries. This is the only
// lossless way to change a tag's layout, because the encoder can only write
// fields the decoder models, and a real tag carries more than that.
void testRebuildKeepsMain() {
    gCurrentTest = "rebuild keeps main";

    // Fixture 01 is the specification's own PLA Prusa Galaxy Black, and it
    // carries three fields this firmware has no use for.
    std::vector<uint8_t> image;
    if (!readFile("fixtures/01_data.bin", image)) {
        check(false, "fixture 01 readable");
        return;
    }

    opt::OptTag before;
    if (opt::optParse(image.data(), image.size(), before) != opt::OptStatus::Ok) {
        check(false, "fixture parses");
        return;
    }
    check(before.main.unknownFieldCount > 0,
          "the fixture has fields the firmware does not model",
          std::to_string(before.main.unknownFieldCount).c_str());

    const uint8_t *span = nullptr;
    size_t spanLen = 0;
    check(opt::optMainRegionSpan(image.data(), image.size(), before, span,
                                 spanLen),
          "main region span located");
    check(spanLen <= before.regions.mainSize,
          "span excludes any padding after the map");

    std::vector<uint8_t> mainCopy(span, span + spanLen);
    opt::OptBuildRequest request;
    request.mainVerbatim = mainCopy.data();
    request.mainVerbatimLen = mainCopy.size();
    request.tagCapacity = opt::kSlix2Bytes;
    request.minAuxSize = 32;
    request.writeConsumedWeight = true;
    snprintf(request.workgroup, sizeof(request.workgroup), "dryer1");

    uint8_t rebuilt[opt::kMaxTagImage];
    size_t rebuiltLen = 0;
    const opt::OptStatus status =
        opt::optBuildImage(request, rebuilt, sizeof(rebuilt), rebuiltLen,
                           nullptr);
    check(status == opt::OptStatus::Ok, "rebuilds", opt::optStatusText(status));
    if (status != opt::OptStatus::Ok) {
        return;
    }

    opt::OptTag after;
    check(opt::optParse(rebuilt, rebuiltLen, after) == opt::OptStatus::Ok,
          "rebuilt image parses");

    // The main region is the same bytes, so every field is the same whether or
    // not this firmware understands it. The unrecognised count is the proof:
    // had they been dropped, it would read zero.
    check(after.regions.mainSize >= spanLen, "main region still fits");
    check(memcmp(rebuilt + after.regions.mainAbsolute(), mainCopy.data(),
                 mainCopy.size()) == 0,
          "main region copied byte for byte");
    checkEqualInt(after.main.unknownFieldCount, before.main.unknownFieldCount,
                  "fields the firmware cannot model survived");
    checkEqualText(after.main.materialName, before.main.materialName,
                   "material name survived");
    checkEqualInt(after.main.chamberTemperature, before.main.chamberTemperature,
                  "chamber advice survived");
    checkEqualInt(static_cast<long long>(after.main.gtin),
                  static_cast<long long>(before.main.gtin), "gtin survived");

    // And the point of the exercise: somewhere to write consumed weight.
    check(after.regions.hasAux, "an aux region now exists");
    check(after.regions.auxSize >= 32, "aux region is usable");
    check(after.aux.hasConsumedWeight, "seeded with a consumed weight");
    checkEqualText(after.aux.workgroup, "dryer1", "and this device's workgroup");

    // The URI record from the original is not carried: it lives outside the
    // main region, and dropping it is a deliberate trade for aux space.
    check(before.hasUriRecord, "fixture had a uri record");
    check(!after.hasUriRecord, "rebuild drops the uri record");
}

void testBuildRoundTrip() {
    gCurrentTest = "build round trip";

    opt::OptBuildRequest request;
    request.main = prusamentPla();
    request.tagCapacity = opt::kSlix2Bytes;  // 316 B
    request.minAuxSize = 32;
    request.consumedWeight = 0.0f;
    snprintf(request.workgroup, sizeof(request.workgroup), "dryer1");

    uint8_t image[opt::kMaxTagImage];
    size_t imageLen = 0;
    opt::OptRegions built;
    const opt::OptStatus status =
        opt::optBuildImage(request, image, sizeof(image), imageLen, &built);
    check(status == opt::OptStatus::Ok, "builds", opt::optStatusText(status));
    if (status != opt::OptStatus::Ok) {
        return;
    }

    // A 316-byte tag holds a 312-byte image, because the capability container
    // expresses its length in 8-byte units. The reference implementation lands
    // on the same 269-byte payload.
    checkEqualInt(static_cast<long long>(imageLen), 312, "image size");
    checkEqualInt(built.payloadSize, 269, "payload size");
    checkEqualInt(image[0], 0xE1, "cc magic");
    checkEqualInt(image[2], 39, "cc MLEN");
    checkEqualInt(image[imageLen - 1], opt::kTlvTerminator, "tlv terminator");
    check(built.auxAbsolute() % opt::kBlockSize == 0,
          "aux region block aligned");
    check(built.auxSize >= 32, "aux region at least 32 bytes");

    opt::OptTag tag;
    const opt::OptStatus parsed = opt::optParse(image, imageLen, tag);
    check(parsed == opt::OptStatus::Ok, "re-parses",
          opt::optStatusText(parsed));
    if (parsed != opt::OptStatus::Ok) {
        return;
    }

    check(!tag.hasUriRecord, "no uri record by default");
    checkEqualInt(tag.regions.payloadSize, built.payloadSize,
                  "payload size survives");
    checkEqualInt(tag.regions.auxOffset, built.auxOffset,
                  "aux offset survives");
    checkEqualText(tag.main.materialName, "PLA Prusa Galaxy Black",
                   "material name survives");
    checkEqualText(tag.main.brandName, "Prusament", "brand name survives");
    checkEqualText(tag.main.brandSpecificInstanceId, "334c54f088",
                   "spool id survives");
    checkEqualFloat(tag.main.actualNettoFullWeight, 1044.0,
                    "actual net weight survives");
    checkEqualFloat(tag.main.emptyContainerWeight, 201.0,
                    "empty spool weight survives");
    checkEqualFloat(tag.main.actualFullLength, 343000.0, "length survives");
    checkEqualFloat(tag.main.density, 1.24, "density survives");
    checkEqualFloat(tag.main.filamentDiameter, 1.75, "diameter survives");
    checkEqualInt(tag.main.dryingTemperature, 55, "drying temp survives");
    checkEqualInt(tag.main.dryingTime, 420, "drying time survives");
    checkEqualInt(tag.main.minPrintTemperature, 205, "min print temp survives");
    checkEqualInt(tag.main.tagCount, 1, "tag count survives");
    checkEqualInt(tag.main.tags[0], 23, "material tag survives");
    checkEqualInt(tag.main.primaryColorLen, 4, "colour length survives");
    checkEqualInt(tag.main.primaryColor[1], 0x3E, "colour survives");
    checkEqualInt(tag.main.unknownFieldCount, 0, "no unknown fields");

    check(tag.aux.hasConsumedWeight, "aux consumed weight present");
    checkEqualFloat(tag.aux.consumedWeight, 0.0, "aux consumed weight zero");
    checkEqualText(tag.aux.workgroup, "dryer1", "workgroup survives");

    // Unused space is zero filled on initialisation.
    size_t zeros = 0;
    for (size_t i = built.payloadStart + built.auxOffset + 12;
         i < built.payloadStart + built.payloadSize; ++i) {
        if (image[i] == 0) {
            ++zeros;
        }
    }
    check(zeros > 0, "trailing aux space zero filled");
}

void testBuildWithUri() {
    gCurrentTest = "build with uri record";

    opt::OptBuildRequest request;
    request.main = prusamentPla();
    request.tagCapacity = opt::kSlix2Bytes;
    request.uri = "https://prusament.com/spool/?spoolId=334c54f088";

    uint8_t image[opt::kMaxTagImage];
    size_t imageLen = 0;
    opt::OptRegions built;
    const opt::OptStatus status =
        opt::optBuildImage(request, image, sizeof(image), imageLen, &built);
    check(status == opt::OptStatus::Ok, "builds", opt::optStatusText(status));
    if (status != opt::OptStatus::Ok) {
        return;
    }

    checkEqualInt(static_cast<long long>(imageLen), 312, "image size");
    check(built.payloadSize < 269, "uri record costs payload space");

    opt::OptTag tag;
    const opt::OptStatus parsed = opt::optParse(image, imageLen, tag);
    check(parsed == opt::OptStatus::Ok, "re-parses",
          opt::optStatusText(parsed));
    check(tag.hasUriRecord, "uri record found");
    checkEqualText(tag.main.brandName, "Prusament",
                   "main data still decodes past the uri record");

    // The text has to come back, not just the fact that a record was there.
    // Rewriting a single main field rebuilds the whole image, and the URI record
    // lives outside the payload, so without this the link would quietly vanish.
    checkEqualText(tag.uri, "https://prusament.com/spool/?spoolId=334c54f088",
                   "and the uri survives the abbreviated prefix");

    // Each prefix NDEF abbreviates, plus the unabbreviated case.
    const char *uris[] = {
        "https://prusament.com/s/1",     "http://prusament.com/s/2",
        "https://www.prusament.com/s/3", "http://www.prusament.com/s/4",
        "ftp://example.invalid/s/5",
    };
    for (const char *uri : uris) {
        opt::OptBuildRequest each;
        each.main = prusamentPla();
        each.tagCapacity = opt::kSlix2Bytes;
        each.uri = uri;
        uint8_t eachImage[opt::kMaxTagImage];
        size_t eachLen = 0;
        if (opt::optBuildImage(each, eachImage, sizeof(eachImage), eachLen,
                               nullptr) != opt::OptStatus::Ok) {
            check(false, "builds with uri", uri);
            continue;
        }
        opt::OptTag round;
        if (opt::optParse(eachImage, eachLen, round) != opt::OptStatus::Ok) {
            check(false, "re-parses with uri", uri);
            continue;
        }
        checkEqualText(round.uri, uri, "uri round trips");
    }
}

void testBuildTooSmall() {
    gCurrentTest = "build capacity limits";

    opt::OptBuildRequest request;
    request.main = prusamentPla();
    request.tagCapacity = 112;  // ICODE SLIX, too small for this much data

    uint8_t image[opt::kMaxTagImage];
    size_t imageLen = 0;
    const opt::OptStatus status =
        opt::optBuildImage(request, image, sizeof(image), imageLen, nullptr);
    check(status == opt::OptStatus::BufferTooSmall,
          "small tag rejected rather than truncated");

    // An output buffer smaller than the tag is also refused.
    opt::OptBuildRequest big;
    big.main = prusamentPla();
    big.tagCapacity = opt::kSlix2Bytes;
    uint8_t small[64];
    check(opt::optBuildImage(big, small, sizeof(small), imageLen, nullptr) ==
              opt::OptStatus::BufferTooSmall,
          "undersized output buffer rejected");
}

void testAuxUpdate() {
    gCurrentTest = "aux update";

    // An auxiliary region holding a field this firmware does not know about,
    // which an update must carry over untouched.
    uint8_t original[48];
    opt::CborWriter writer(original, sizeof(original));
    writer.writeIndefiniteMap();
    writer.writeUnsigned(opt::aux_key::kConsumedWeight);
    writer.writeNumber(444.0);
    writer.writeUnsigned(opt::aux_key::kWorkgroup);
    writer.writeText("myhome");
    writer.writeUnsigned(9999);       // unknown key
    writer.writeText("keepme");
    writer.writeBreak();
    check(writer.ok(), "seed region encoded");
    const size_t originalLen = writer.length();

    opt::OptAuxUpdate update;
    update.setConsumedWeight = true;
    update.consumedWeight = 512.0f;

    uint8_t updated[48];
    size_t updatedLen = 0;
    check(opt::optBuildAuxRegion(original, originalLen, update, updated,
                                 sizeof(updated),
                                 updatedLen) == opt::OptStatus::Ok,
          "update encodes");

    opt::OptAuxData aux;
    check(opt::optParseAux(updated, updatedLen, aux) == opt::OptStatus::Ok,
          "updated region parses");
    checkEqualFloat(aux.consumedWeight, 512.0, "consumed weight updated");
    checkEqualText(aux.workgroup, "myhome", "workgroup untouched");
    checkEqualInt(aux.unknownFieldCount, 1, "unknown field preserved");

    // The unknown field's bytes survive verbatim.
    const std::string updatedHex = toHex(updated, updatedLen);
    check(updatedHex.find("666b6565706d65") != std::string::npos,
          "unknown value bytes preserved");

    // Updating in place keeps the region's tail stable, so the block diff stays
    // small instead of rewriting everything after the changed field.
    size_t firstBlock = 0;
    size_t blockCount = 0;
    check(opt::optDiffBlocks(original, updated,
                             originalLen < updatedLen ? originalLen : updatedLen,
                             opt::kBlockSize, firstBlock, blockCount),
          "diff found");
    check(blockCount <= 2, "diff limited to the changed field's blocks",
          std::to_string(blockCount) + " blocks");

    // A wipe drops unknown content, for the workgroup mismatch case.
    opt::OptAuxUpdate wipe;
    wipe.setConsumedWeight = true;
    wipe.consumedWeight = 0.0f;
    wipe.setWorkgroup = true;
    snprintf(wipe.workgroup, sizeof(wipe.workgroup), "dryer1");
    wipe.wipeUnknown = true;

    uint8_t wiped[48];
    size_t wipedLen = 0;
    check(opt::optBuildAuxRegion(original, originalLen, wipe, wiped,
                                 sizeof(wiped),
                                 wipedLen) == opt::OptStatus::Ok,
          "wipe encodes");
    opt::OptAuxData wipedAux;
    check(opt::optParseAux(wiped, wipedLen, wipedAux) == opt::OptStatus::Ok,
          "wiped region parses");
    checkEqualInt(wipedAux.unknownFieldCount, 0, "unknown field dropped");
    checkEqualText(wipedAux.workgroup, "dryer1", "workgroup replaced");
    checkEqualFloat(wipedAux.consumedWeight, 0.0, "consumed weight reset");

    // Adding a field to an empty region works too.
    const uint8_t empty[] = {0xA0};
    opt::OptAuxUpdate seed;
    seed.setConsumedWeight = true;
    seed.consumedWeight = 17.0f;
    seed.setWorkgroup = true;
    snprintf(seed.workgroup, sizeof(seed.workgroup), "dryer1");
    uint8_t seeded[32];
    size_t seededLen = 0;
    check(opt::optBuildAuxRegion(empty, sizeof(empty), seed, seeded,
                                 sizeof(seeded),
                                 seededLen) == opt::OptStatus::Ok,
          "empty region seeded");
    opt::OptAuxData seededAux;
    check(opt::optParseAux(seeded, seededLen, seededAux) == opt::OptStatus::Ok,
          "seeded region parses");
    checkEqualFloat(seededAux.consumedWeight, 17.0, "seeded consumed weight");
    checkEqualText(seededAux.workgroup, "dryer1", "seeded workgroup");

    // No room means an error, never a partial write.
    uint8_t cramped[4];
    size_t crampedLen = 0;
    check(opt::optBuildAuxRegion(original, originalLen, update, cramped,
                                 sizeof(cramped), crampedLen) ==
              opt::OptStatus::BufferTooSmall,
          "cramped output rejected");
}

void testAuxUpdateOnFixture() {
    gCurrentTest = "aux update on fixture 03";

    std::vector<uint8_t> image;
    if (!readFile("fixtures/03_data.bin", image)) {
        check(false, "fixture readable");
        return;
    }

    opt::OptTag tag;
    check(opt::optParse(image.data(), image.size(), tag) == opt::OptStatus::Ok,
          "fixture parses");
    checkEqualFloat(tag.aux.consumedWeight, 444.0, "starting consumed weight");
    checkEqualText(tag.aux.workgroup, "myhome", "starting workgroup");

    const uint8_t *auxRegion = image.data() + tag.regions.auxAbsolute();
    const size_t auxSize = tag.regions.auxSize;

    // Re-encoding without changes must reproduce the region byte for byte,
    // which is what makes "write only the blocks that differ" safe.
    opt::OptAuxUpdate noop;
    uint8_t rebuilt[64];
    size_t rebuiltLen = 0;
    check(opt::optBuildAuxRegion(auxRegion, auxSize, noop, rebuilt,
                                 sizeof(rebuilt),
                                 rebuiltLen) == opt::OptStatus::Ok,
          "no-op re-encode");
    check(memcmp(auxRegion, rebuilt, rebuiltLen) == 0,
          "no-op re-encode is byte identical",
          toHex(rebuilt, rebuiltLen) + " vs " + toHex(auxRegion, rebuiltLen));

    size_t firstBlock = 0;
    size_t blockCount = 0;
    check(!opt::optDiffBlocks(auxRegion, rebuilt, rebuiltLen, opt::kBlockSize,
                              firstBlock, blockCount),
          "no-op produces no blocks to write");

    // A real update changes only the value's blocks.
    opt::OptAuxUpdate update;
    update.setConsumedWeight = true;
    update.consumedWeight = 500.0f;
    uint8_t updated[64];
    size_t updatedLen = 0;
    check(opt::optBuildAuxRegion(auxRegion, auxSize, update, updated,
                                 sizeof(updated),
                                 updatedLen) == opt::OptStatus::Ok,
          "update encodes");
    check(opt::optDiffBlocks(auxRegion, updated, updatedLen, opt::kBlockSize,
                             firstBlock, blockCount),
          "update produces a diff");
    check(blockCount <= 2, "update touches at most two blocks",
          std::to_string(blockCount) + " blocks");
}

// A tag is untrusted input: any spool could carry a corrupt or hostile image,
// and the parser has to survive it. Mutating a known-good fixture in a
// reproducible way covers far more of the decoder's error paths than
// hand-written malformed cases.
void testMutationRobustness() {
    gCurrentTest = "mutation robustness";

    std::vector<uint8_t> original;
    if (!readFile("fixtures/01_data.bin", original)) {
        check(false, "fixture readable");
        return;
    }

    uint32_t state = 0x12345678;
    auto nextRandom = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    int parsedOk = 0;
    int rejected = 0;
    bool boundsHeld = true;

    for (int iteration = 0; iteration < 20000; ++iteration) {
        std::vector<uint8_t> image = original;
        const int mutations = 1 + static_cast<int>(nextRandom() % 6);
        for (int i = 0; i < mutations; ++i) {
            const size_t position = nextRandom() % image.size();
            image[position] = static_cast<uint8_t>(nextRandom());
        }
        // Sometimes hand over a short buffer as well, mimicking a read that
        // stopped early because the tag left the field.
        const size_t length = (nextRandom() % 4 == 0)
                                  ? (1 + nextRandom() % image.size())
                                  : image.size();

        opt::OptTag tag;
        if (opt::optParse(image.data(), length, tag) == opt::OptStatus::Ok) {
            ++parsedOk;
            // Whatever it accepted, the regions it reports must sit inside the
            // buffer it was given.
            const size_t mainEnd = tag.regions.mainAbsolute() +
                                   tag.regions.mainSize;
            const size_t auxEnd = tag.regions.hasAux
                                      ? tag.regions.auxAbsolute() +
                                            tag.regions.auxSize
                                      : 0;
            if (mainEnd > length || auxEnd > length) {
                boundsHeld = false;
            }
        } else {
            ++rejected;
        }
    }

    check(boundsHeld, "reported regions stay inside the buffer");
    check(parsedOk > 0, "some mutations still parse");
    check(rejected > 0, "some mutations are rejected");
}

void testDiffBlocks() {
    gCurrentTest = "block diff";

    uint8_t before[16] = {0};
    uint8_t after[16] = {0};
    size_t firstBlock = 0;
    size_t blockCount = 0;

    check(!opt::optDiffBlocks(before, after, sizeof(before), 4, firstBlock,
                              blockCount),
          "identical buffers need no write");

    after[5] = 0x01;
    check(opt::optDiffBlocks(before, after, sizeof(before), 4, firstBlock,
                             blockCount),
          "single change found");
    checkEqualInt(static_cast<long long>(firstBlock), 1, "first block");
    checkEqualInt(static_cast<long long>(blockCount), 1, "block count");

    after[13] = 0x02;
    check(opt::optDiffBlocks(before, after, sizeof(before), 4, firstBlock,
                             blockCount),
          "spanning change found");
    checkEqualInt(static_cast<long long>(firstBlock), 1, "span first block");
    checkEqualInt(static_cast<long long>(blockCount), 3, "span block count");
}

}  // namespace

int main() {
    printf("OpenPrintTag codec tests\n");

    testCborPrimitives();
    testMalformedInput();
    testFixtures();
    testSampleTag();
    testBuildRoundTrip();
    testCarriedFields();
    testRebuildKeepsMain();
    testBuildWithUri();
    testBuildTooSmall();
    testAuxUpdate();
    testAuxUpdateOnFixture();
    testMutationRobustness();
    testDiffBlocks();

    printf("\n%d checks, %d failure%s\n", gChecks, gFailures,
           gFailures == 1 ? "" : "s");
    return gFailures == 0 ? 0 : 1;
}
