#include "PrusamentPresets.h"

#include <string.h>

namespace presets {
namespace {

// Tag enum values used below: 12 blend, 16 matte, 17 silk, 23 glitter,
// 30 contains_carbon, 31 contains_carbon_fiber, 60 recycled,
// 6 self_extinguishing, 9 high_temperature.
// Print temperatures for the PLA and PETG families are the ones Prusa's own
// tags carry -- 205-225 for PLA, 240-260 for PETG -- confirmed against a
// factory-provisioned Prusament PLA Galaxy Black spool and the specification's
// example fixtures, both of which are more authoritative here than the nominal
// figure on a data sheet.
constexpr MaterialPreset kPresets[] = {
    {"pla", "Prusament PLA", "PLA", "PLA", 0, 1.24f,
     205, 225, 170, 40, 60, 45, 360, {0, 0, 0}, 0},

    {"pla_blend", "Prusament PLA Blend", "PLA Blend", "PLA", 0, 1.24f,
     205, 225, 170, 40, 60, 45, 360, {12, 0, 0}, 1},

    {"rpla", "Prusament rPLA", "rPLA", "rPLA", 0, 1.24f,
     205, 225, 170, 40, 60, 45, 360, {60, 0, 0}, 1},

    // The name prefix matters beyond cosmetics: material_name is what
    // material_uuid is derived from, so "PLA Prusa" + "Galaxy Black" has to
    // reproduce Prusa's own "PLA Prusa Galaxy Black" exactly or other tools
    // will treat a spool this device provisioned as a different material.
    {"pla_galaxy", "Prusament PLA (Galaxy)", "PLA Prusa", "PLA", 0, 1.24f,
     205, 225, 170, 40, 60, 45, 360, {23, 0, 0}, 1},

    // Wood-filled PLA, so material_type stays PLA. Unlike the PLA family above,
    // these temperatures are the published ones (extruder 195 +/- 10, bed
    // 60 +/- 10) rather than a factory tag's, because Prusa ships Woodfill
    // without a tag -- worth re-checking against one if that changes. Density
    // 1.16 is from the data sheet, and it is the wood that makes it lighter than
    // the 1.24 of plain PLA.
    //
    // Prusa says Woodfill does not need drying. The PLA family's 45 C is kept
    // anyway so the dry-cycle script has something to work with if you want to,
    // and 45 C is well under this material's 62.6 C heat deflection point.
    //
    // The abbreviation is capped at 7 characters by the specification, so
    // "PLA-Wood" will not fit; "PLA-W" keeps the base polymer visible, which
    // plain "PLA" would not.
    {"woodfill", "Prusament Woodfill", "Woodfill", "PLA-W", 0, 1.16f,
     185, 205, 170, 50, 70, 45, 360, {12, 0, 0}, 1},

    {"petg", "Prusament PETG", "PETG", "PETG", 1, 1.27f,
     240, 260, 170, 70, 90, 55, 360, {0, 0, 0}, 0},

    {"petg_v0", "Prusament PETG V0", "PETG V0", "PETG", 1, 1.27f,
     240, 260, 170, 70, 90, 55, 360, {6, 0, 0}, 1},

    {"petg_cf", "Prusament PETG Carbon Fiber", "PETG Carbon Fiber", "PETG-CF",
     1, 1.27f, 240, 260, 170, 80, 90, 55, 360, {30, 31, 0}, 2},

    {"pvb", "Prusament PVB", "PVB", "PVB", 19, 1.10f,
     205, 225, 170, 70, 80, 45, 480, {14, 19, 0}, 2},

    {"asa", "Prusament ASA", "ASA", "ASA", 4, 1.07f,
     255, 265, 170, 105, 115, 80, 240, {0, 0, 0}, 0},

    {"pc_blend", "Prusament PC Blend", "PC Blend", "PC", 5, 1.22f,
     265, 285, 240, 100, 120, 85, 300, {9, 12, 0}, 2},

    {"pc_blend_cf", "Prusament PC Blend Carbon Fiber", "PC Blend Carbon Fiber",
     "PC-CF", 5, 1.22f, 265, 285, 240, 100, 120, 90, 240, {9, 30, 31}, 3},

    {"pa11_cf", "Prusament PA11 Carbon Fiber", "PA11 Carbon Fiber", "PA11-CF",
     9, 1.11f, 280, 290, 240, 100, 120, 90, 360, {30, 31, 9}, 3},
};

constexpr ColorPreset kColors[] = {
    {"Galaxy Black", {0x3D, 0x3E, 0x3D, 0xFF}},
    {"Jet Black", {0x24, 0x29, 0x2A, 0xFF}},
    {"Prusa Orange", {0xFF, 0x6B, 0x00, 0xFF}},
    {"Prusa Galaxy Silver", {0x8E, 0x91, 0x96, 0xFF}},
    {"Vanilla White", {0xF7, 0xF3, 0xE8, 0xFF}},
    {"Lipstick Red", {0xB5, 0x1A, 0x28, 0xFF}},
    {"Ms. Pink", {0xF2, 0x60, 0xA1, 0xFF}},
    {"Azure Blue", {0x00, 0x71, 0xBC, 0xFF}},
    {"Mystic Green", {0x1E, 0x5B, 0x3E, 0xFF}},
    {"Gravity Grey", {0x5B, 0x5F, 0x63, 0xFF}},
    {"Custom", {0x80, 0x80, 0x80, 0xFF}},
};

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

const MaterialPreset *all(size_t &count) {
    count = sizeof(kPresets) / sizeof(kPresets[0]);
    return kPresets;
}

const MaterialPreset *byId(const char *id) {
    if (id == nullptr) {
        return nullptr;
    }
    for (const MaterialPreset &preset : kPresets) {
        if (strcmp(preset.id, id) == 0) {
            return &preset;
        }
    }
    return nullptr;
}

const ColorPreset *colors(size_t &count) {
    count = sizeof(kColors) / sizeof(kColors[0]);
    return kColors;
}

const ColorPreset *colorByName(const char *name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (const ColorPreset &color : kColors) {
        if (strcmp(color.name, name) == 0) {
            return &color;
        }
    }
    return nullptr;
}

bool parseColor(const char *text, uint8_t rgba[4]) {
    if (text == nullptr) {
        return false;
    }
    if (*text == '#') {
        ++text;
    }
    const size_t length = strlen(text);
    if (length != 6 && length != 8) {
        return false;
    }

    for (size_t i = 0; i < length; i += 2) {
        const int high = hexDigit(text[i]);
        const int low = hexDigit(text[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        rgba[i / 2] = static_cast<uint8_t>((high << 4) | low);
    }
    if (length == 6) {
        rgba[3] = 0xFF;
    }
    return true;
}

void applyToMain(const MaterialPreset &preset, const char *colorName,
                 const uint8_t rgba[4], opt::OptMainData &main) {
    main.hasMaterialClass = true;
    main.materialClass = 0;  // FFF
    main.hasMaterialType = true;
    main.materialType = preset.materialType;

    snprintf(main.brandName, sizeof(main.brandName), "Prusament");
    snprintf(main.materialAbbreviation, sizeof(main.materialAbbreviation), "%s",
             preset.abbreviation);

    // Prusa's own tags name the material "<type> <colour>", for example
    // "PETG Jet Black", so follow that.
    if (colorName != nullptr && colorName[0] != '\0') {
        snprintf(main.materialName, sizeof(main.materialName), "%s %s",
                 preset.namePrefix, colorName);
    } else {
        snprintf(main.materialName, sizeof(main.materialName), "%s",
                 preset.namePrefix);
    }

    main.hasDensity = true;
    main.density = preset.density;
    main.hasFilamentDiameter = true;
    main.filamentDiameter = kFilamentDiameterMm;

    main.hasMinPrintTemperature = true;
    main.minPrintTemperature = preset.minPrintC;
    main.hasMaxPrintTemperature = true;
    main.maxPrintTemperature = preset.maxPrintC;
    main.hasPreheatTemperature = true;
    main.preheatTemperature = preset.preheatC;
    main.hasMinBedTemperature = true;
    main.minBedTemperature = preset.minBedC;
    main.hasMaxBedTemperature = true;
    main.maxBedTemperature = preset.maxBedC;

    main.hasDryingTemperature = true;
    main.dryingTemperature = preset.dryingC;
    main.hasDryingTime = true;
    main.dryingTime = preset.dryingMinutes;

    main.hasNominalNettoFullWeight = true;
    main.nominalNettoFullWeight = kPrusamentNominalGrams;

    if (rgba != nullptr) {
        main.hasPrimaryColor = true;
        memcpy(main.primaryColor, rgba, 4);
        main.primaryColorLen = 4;
    }

    main.tagCount = 0;
    for (uint8_t i = 0; i < preset.tagCount && i < opt::kMaxMaterialTags; ++i) {
        main.tags[main.tagCount++] = preset.tags[i];
    }

    // Our own tags are never locked: a typo should be fixable by rewriting.
    main.hasWriteProtection = true;
    main.writeProtection = 0;
}

}  // namespace presets
