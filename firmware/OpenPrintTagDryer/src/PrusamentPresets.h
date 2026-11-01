// Per-material constants for Prusament, mapped onto the OpenPrintTag enums.
//
// Provisioning a spool needs two kinds of data. The per-spool part (measured
// filament weight, spool weight, length, spool ID) comes from that spool's page
// at prusament.com and has to be entered. Everything else is a property of the
// material, is the same on every spool of it, and lives here.
//
// Sources, all Prusa's own published figures:
//   density, print and bed temperatures  material technical data sheets
//                                        (prusament.com/wp-content/uploads/...)
//   drying temperature and time          Prusa Knowledge Base, "Drying
//                                        filament" (help.prusa3d.com, 332086)
//
// The Creality Space Pi tops out at 70 C, so anything above that is flagged:
// the tag still records what Prusa recommends, and the dryer does what it can.

#pragma once

#include <Arduino.h>

#include "openprinttag/opt_tag.h"

namespace presets {

// A hint for the provisioning form, never a value anything is calculated from.
// Spool weights differ between generations: a 2025 Prusament spool reports
// 186 g on its own tag, older ones 201 g, and the specification's example
// fixtures use 280 g. Only the figure on that spool's page is trustworthy.
constexpr float kPrusamentSpoolGrams = 186.0f;
constexpr float kPrusamentNominalGrams = 1000.0f;
constexpr float kFilamentDiameterMm = 1.75f;

// Highest air temperature the dryer can reach.
constexpr int16_t kDryerMaxTemperatureC = 70;

struct MaterialPreset {
    const char *id;             // stable slug, used by the web form
    const char *displayName;    // "Prusament PETG"
    const char *namePrefix;     // material_name becomes "<prefix> <colour>"
    const char *abbreviation;   // material_abbreviation on the tag
    uint16_t materialType;      // specification material_type enum
    float density;              // g/cm3
    int16_t minPrintC;
    int16_t maxPrintC;
    int16_t preheatC;
    int16_t minBedC;
    int16_t maxBedC;
    int16_t dryingC;
    int16_t dryingMinutes;
    uint16_t tags[3];
    uint8_t tagCount;
};

// A handful of Prusament colours with the hex values Prusa publishes. The
// provisioning form also accepts a free-form name and hex, since Prusament
// colours come and go faster than any table can track.
struct ColorPreset {
    const char *name;
    uint8_t rgba[4];
};

const MaterialPreset *all(size_t &count);
const MaterialPreset *byId(const char *id);
const ColorPreset *colors(size_t &count);
const ColorPreset *colorByName(const char *name);

// Parses "#rrggbb" or "#rrggbbaa" (with or without the hash).
bool parseColor(const char *text, uint8_t rgba[4]);

// True when the material wants a hotter dry than this dryer can deliver.
inline bool needsHotterDryer(const MaterialPreset &preset) {
    return preset.dryingC > kDryerMaxTemperatureC;
}

// Fills in everything a tag's main region should hold for a spool of this
// material, leaving the per-spool measured values to the caller.
void applyToMain(const MaterialPreset &preset, const char *colorName,
                 const uint8_t rgba[4], opt::OptMainData &main);

}  // namespace presets
