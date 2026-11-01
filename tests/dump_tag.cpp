// Decode a tag image off the desktop, using the firmware's own codec.
//
//     make -C tests dump_tag
//     tests/dump_tag tag_E0_04_01_50_XX_XX_XX_XX.bin
//
// The portal's Tag page shows the same information, but only while the tag is
// in the field and only as far as the firmware understands it. This prints
// every recognised field and counts the ones it does not, which is what you
// want when the question is whether another implementation's tag decodes
// correctly here. Dumps come from Download these bytes on the Tag page.
//
// A non-zero exit status means the image did not parse, so this is usable as a
// check over a directory of collected tags.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <vector>

#include "../firmware/OpenPrintTagDryer/src/openprinttag/opt_enums.h"
#include "../firmware/OpenPrintTagDryer/src/openprinttag/opt_tag.h"

namespace {

bool readFile(const char *path, std::vector<uint8_t> &out) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    uint8_t chunk[256];
    size_t got = 0;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        out.insert(out.end(), chunk, chunk + got);
    }
    fclose(file);
    return true;
}

void printText(const char *label, const char *value) {
    if (value[0] != '\0') {
        printf("  %-26s %s\n", label, value);
    }
}

void printFloat(const char *label, bool has, float value, const char *unit) {
    if (has) {
        printf("  %-26s %.3f %s\n", label, value, unit);
    }
}

void printInt(const char *label, bool has, long value, const char *unit) {
    if (has) {
        printf("  %-26s %ld %s\n", label, value, unit);
    }
}

// The spec's timestamp type is seconds since the Unix epoch. Printed as a date
// as well as the raw number, because a plausible-looking integer is exactly how
// a units mistake hides.
void printTimestamp(const char *label, bool has, uint32_t value) {
    if (!has) {
        return;
    }
    const time_t when = static_cast<time_t>(value);
    struct tm utc;
    char text[32] = "unrepresentable";
    if (gmtime_r(&when, &utc) != nullptr) {
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M UTC", &utc);
    }
    printf("  %-26s %s (%lu)\n", label, text,
           static_cast<unsigned long>(value));
}

// 8-4-4-4-12, the form every other tool prints a UUID in.
void printUuid(const char *label, bool has, const uint8_t *value) {
    if (!has) {
        return;
    }
    char text[40];
    snprintf(text, sizeof(text),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             value[0], value[1], value[2], value[3], value[4], value[5],
             value[6], value[7], value[8], value[9], value[10], value[11],
             value[12], value[13], value[14], value[15]);
    printf("  %-26s %s (stored)\n", label, text);
}

void printMain(const opt::OptMainData &main) {
    printf("Main region\n");
    printUuid("instance_uuid", main.hasInstanceUuid, main.instanceUuid);
    printUuid("package_uuid", main.hasPackageUuid, main.packageUuid);
    printUuid("material_uuid", main.hasMaterialUuid, main.materialUuid);
    printUuid("brand_uuid", main.hasBrandUuid, main.brandUuid);
    printText("material_name", main.materialName);
    printText("material_abbreviation", main.materialAbbreviation);
    printText("brand_name", main.brandName);
    printText("brand_specific_instance_id", main.brandSpecificInstanceId);
    if (main.hasMaterialType) {
        printf("  %-26s %u (%s)\n", "material_type", main.materialType,
               opt::materialTypeName(main.materialType));
    }
    if (main.hasMaterialClass) {
        printf("  %-26s %u (%s)\n", "material_class", main.materialClass,
               opt::materialClassName(main.materialClass));
    }
    if (main.hasGtin) {
        printf("  %-26s %llu\n", "gtin",
               static_cast<unsigned long long>(main.gtin));
    }
    printFloat("nominal_netto_full_weight", main.hasNominalNettoFullWeight,
               main.nominalNettoFullWeight, "g");
    printFloat("actual_netto_full_weight", main.hasActualNettoFullWeight,
               main.actualNettoFullWeight, "g");
    printFloat("empty_container_weight", main.hasEmptyContainerWeight,
               main.emptyContainerWeight, "g");
    printFloat("nominal_full_length", main.hasNominalFullLength,
               main.nominalFullLength / 1000.0f, "m");
    printFloat("actual_full_length", main.hasActualFullLength,
               main.actualFullLength / 1000.0f, "m");
    printFloat("density", main.hasDensity, main.density, "g/cm3");
    printFloat("filament_diameter", main.hasFilamentDiameter,
               main.filamentDiameter, "mm");
    if (main.hasPrimaryColor) {
        printf("  %-26s #%02X%02X%02X", "primary_color", main.primaryColor[0],
               main.primaryColor[1], main.primaryColor[2]);
        if (main.primaryColorLen > 3) {
            printf(" alpha %u", main.primaryColor[3]);
        }
        printf("\n");
    }
    printInt("min_print_temperature", main.hasMinPrintTemperature,
             main.minPrintTemperature, "C");
    printInt("max_print_temperature", main.hasMaxPrintTemperature,
             main.maxPrintTemperature, "C");
    printInt("preheat_temperature", main.hasPreheatTemperature,
             main.preheatTemperature, "C");
    printInt("min_bed_temperature", main.hasMinBedTemperature,
             main.minBedTemperature, "C");
    printInt("max_bed_temperature", main.hasMaxBedTemperature,
             main.maxBedTemperature, "C");
    printInt("min_chamber_temperature", main.hasMinChamberTemperature,
             main.minChamberTemperature, "C");
    printInt("max_chamber_temperature", main.hasMaxChamberTemperature,
             main.maxChamberTemperature, "C");
    printInt("chamber_temperature", main.hasChamberTemperature,
             main.chamberTemperature, "C");
    printInt("drying_temperature", main.hasDryingTemperature,
             main.dryingTemperature, "C");
    printInt("drying_time", main.hasDryingTime, main.dryingTime, "min");
    printInt("container_width", main.hasContainerWidth, main.containerWidth,
             "mm");
    printInt("container_outer_diameter", main.hasContainerOuterDiameter,
             main.containerOuterDiameter, "mm");
    printInt("container_inner_diameter", main.hasContainerInnerDiameter,
             main.containerInnerDiameter, "mm");
    printInt("container_hole_diameter", main.hasContainerHoleDiameter,
             main.containerHoleDiameter, "mm");
    printTimestamp("manufactured_date", main.hasManufacturedDate,
                   main.manufacturedDate);
    printTimestamp("expiration_date", main.hasExpirationDate,
                   main.expirationDate);
    if (main.hasWriteProtection) {
        printf("  %-26s %u (%s)\n", "write_protection", main.writeProtection,
               opt::writeProtectionName(main.writeProtection));
    }
    if (main.tagCount > 0) {
        printf("  %-26s", "tags");
        for (uint8_t i = 0; i < main.tagCount; ++i) {
            printf(" %s", opt::materialTagName(main.tags[i]));
        }
        if (main.tagsTruncated) {
            printf(" (list truncated)");
        }
        printf("\n");
    }
    // Not an error: these are fields the specification defines and this
    // firmware has no use for. It matters only because a full rewrite of the
    // main region drops them, so it is the size of what a re-provision costs.
    printf("  %-26s %u\n", "unrecognised fields", main.unknownFieldCount);
    if (!main.hasInstanceUuid && !main.hasBrandUuid) {
        printf("  %-26s derived, not stored, per spec 3.2\n", "identity uuids");
    }
}

void printAux(const opt::OptTag &tag) {
    printf("Auxiliary region\n");
    if (!tag.regions.hasAux) {
        printf("  none declared, so nothing can be written to this tag\n");
        return;
    }
    if (tag.auxStatus != opt::OptStatus::Ok) {
        printf("  malformed: %s\n", opt::optStatusText(tag.auxStatus));
        return;
    }
    const opt::OptAuxData &aux = tag.aux;
    printFloat("consumed_weight", aux.hasConsumedWeight, aux.consumedWeight,
               "g");
    printText("workgroup", aux.workgroup);
    printText("storage_location", aux.storageLocation);
    printTimestamp("purchase_time", aux.hasPurchaseTime, aux.purchaseTime);
    printFloat("purchase_price", aux.hasPurchasePrice, aux.purchasePrice,
               aux.purchaseCurrency[0] != '\0' ? aux.purchaseCurrency : "");
    printf("  %-26s %u\n", "unrecognised fields", aux.unknownFieldCount);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: dump_tag <image.bin> [more.bin ...]\n");
        return 2;
    }

    int failures = 0;
    for (int arg = 1; arg < argc; ++arg) {
        const char *path = argv[arg];
        std::vector<uint8_t> image;
        if (!readFile(path, image)) {
            fprintf(stderr, "%s: cannot read\n", path);
            ++failures;
            continue;
        }

        printf("%s\n%zu bytes", path, image.size());
        if (image.size() % opt::kBlockSize != 0) {
            printf("  (not a whole number of %zu-byte blocks, so the dump is "
                   "probably truncated)",
                   opt::kBlockSize);
        }
        printf("\n\n");

        opt::OptTag tag;
        const opt::OptStatus status = opt::optParse(image.data(), image.size(), tag);
        if (status != opt::OptStatus::Ok) {
            printf("does not parse: %s\n\n", opt::optStatusText(status));
            ++failures;
            continue;
        }

        printf("Layout\n");
        printf("  %-26s %u B declared\n", "capability container",
               tag.regions.ccCapacity);
        printf("  %-26s %u B at %u\n", "payload", tag.regions.payloadSize,
               tag.regions.payloadStart);
        printf("  %-26s %u B\n", "meta", tag.regions.metaSize);
        printf("  %-26s %u B at %u\n", "main", tag.regions.mainSize,
               tag.regions.mainAbsolute());
        if (tag.regions.hasAux) {
            printf("  %-26s %u B at %u\n", "aux", tag.regions.auxSize,
                   tag.regions.auxAbsolute());
        }
        if (tag.hasUriRecord) {
            printf("  %-26s yes\n", "leading URI record");
        }
        printf("\n");
        printMain(tag.main);
        printf("\n");
        printAux(tag);
        printf("\n");
    }

    return failures == 0 ? 0 : 1;
}
