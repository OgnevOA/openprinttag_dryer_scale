#include "WebPortal.h"

#include <Update.h>

#include "openprinttag/opt_enums.h"

namespace {

// Small, dark, and readable on a phone held over a dryer. No external fonts or
// scripts: the device may well be on a network with no route to the internet.
const char kStyle[] =
    "<style>"
    ":root{--bg:#14161a;--card:#1d2026;--line:#2c3038;--text:#e7e9ee;"
    "--dim:#98a0af;--accent:#4c9be8;--good:#4cc38a;--warn:#e8b64c;--bad:#e05b5b}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--text);"
    "font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}"
    "header{background:var(--card);border-bottom:1px solid var(--line);"
    "padding:12px 16px}"
    "header h1{margin:0 0 8px;font-size:17px;font-weight:600}"
    "nav a{display:inline-block;margin:0 12px 0 0;color:var(--dim);"
    "text-decoration:none;font-size:14px;padding:2px 0;border-bottom:2px solid transparent}"
    "nav a.on{color:var(--text);border-bottom-color:var(--accent)}"
    "main{max-width:760px;margin:0 auto;padding:16px}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:10px;"
    "padding:14px 16px;margin:0 0 14px}"
    ".card h2{margin:0 0 10px;font-size:14px;text-transform:uppercase;"
    "letter-spacing:.06em;color:var(--dim);font-weight:600}"
    ".big{font-size:30px;font-weight:600;line-height:1.1}"
    ".big span{font-size:15px;color:var(--dim);font-weight:400}"
    "table{width:100%;border-collapse:collapse}"
    "td{padding:5px 0;border-bottom:1px solid var(--line);vertical-align:top}"
    "td:first-child{color:var(--dim);width:45%}"
    "tr:last-child td{border-bottom:none}"
    ".bar{height:9px;background:var(--line);border-radius:5px;overflow:hidden;"
    "margin:10px 0 4px}"
    ".bar i{display:block;height:100%;background:var(--accent)}"
    ".sw{display:inline-block;width:13px;height:13px;border-radius:3px;"
    "border:1px solid var(--line);vertical-align:-2px;margin-right:6px}"
    "label{display:block;margin:10px 0 4px;color:var(--dim);font-size:13px}"
    "input,select,button{font:inherit}"
    "input,select{width:100%;padding:8px 10px;border-radius:7px;"
    "border:1px solid var(--line);background:#171a1f;color:var(--text)}"
    "input[type=checkbox]{width:auto}"
    "button{padding:9px 14px;border-radius:7px;border:1px solid var(--line);"
    "background:#252a32;color:var(--text);cursor:pointer;margin:10px 6px 0 0}"
    "button.primary{background:var(--accent);border-color:var(--accent);"
    "color:#0d1117;font-weight:600}"
    "button.danger{border-color:var(--bad);color:var(--bad)}"
    ".row{display:flex;gap:12px;flex-wrap:wrap}.row>div{flex:1 1 150px}"
    ".flash{background:#1f2a38;border:1px solid var(--accent);border-radius:8px;"
    "padding:10px 12px;margin:0 0 14px;font-size:14px}"
    ".ok{color:var(--good)}.warn{color:var(--warn)}.bad{color:var(--bad)}"
    ".dim{color:var(--dim)}"
    "pre{background:#12141a;border:1px solid var(--line);border-radius:8px;"
    "padding:12px;overflow-x:auto;font-size:12.5px;line-height:1.45}"
    "</style>";

struct NavItem {
    const char *path;
    const char *label;
};

constexpr NavItem kNav[] = {
    {"/", "Status"},        {"/provision", "New spool"},
    {"/calibrate", "Scale"}, {"/tag", "Tag"},
    {"/settings", "Settings"}, {"/selftest", "Self-test"},
};

String escape(const char *text) {
    String out;
    if (text == nullptr) {
        return out;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        switch (*p) {
        case '&': out += F("&amp;"); break;
        case '<': out += F("&lt;"); break;
        case '>': out += F("&gt;"); break;
        case '"': out += F("&quot;"); break;
        case '\'': out += F("&#39;"); break;
        default:
            if (static_cast<unsigned char>(*p) < 0x20) {
                out += '.';
            } else {
                out += *p;
            }
        }
    }
    return out;
}

String escape(const String &text) { return escape(text.c_str()); }

void row(String &out, const char *label, const String &value) {
    out += F("<tr><td>");
    out += label;
    out += F("</td><td>");
    out += value;
    out += F("</td></tr>");
}

String grams(float value, int decimals = 1) {
    return String(value, decimals) + " g";
}

}  // namespace

void WebPortal::begin(App &app, const Hooks &hooks, const char *deviceId) {
    app_ = &app;
    hooks_ = hooks;
    snprintf(deviceId_, sizeof(deviceId_), "%s", deviceId);

    server_.on("/", HTTP_GET, [this]() { handleRoot(); });
    server_.on("/provision", HTTP_GET, [this]() { handleProvision(); });
    server_.on("/provision", HTTP_POST, [this]() { handleProvisionPost(); });
    server_.on("/calibrate", HTTP_GET, [this]() { handleCalibrate(); });
    server_.on("/calibrate", HTTP_POST, [this]() { handleCalibratePost(); });
    server_.on("/settings", HTTP_GET, [this]() { handleSettings(); });
    server_.on("/settings", HTTP_POST, [this]() { handleSettingsPost(); });
    server_.on("/tag", HTTP_GET, [this]() { handleTagDump(); });
    server_.on("/tag.bin", HTTP_GET, [this]() { handleTagBin(); });
    server_.on("/selftest", HTTP_GET, [this]() { handleSelfTest(); });
    server_.on("/selftest", HTTP_POST, [this]() { handleReadTest(); });
    server_.on("/drift.csv", HTTP_GET, [this]() { handleDriftCsv(); });
    server_.on("/action", HTTP_POST, [this]() { handleAction(); });
    server_.on("/api/state", HTTP_GET, [this]() { handleStateJson(); });
    server_.on("/update", HTTP_GET, [this]() { handleUpdatePage(); });
    server_.on("/update", HTTP_POST, [this]() { handleUpdateDone(); },
               [this]() { handleUpdateUpload(); });
    server_.onNotFound([this]() { handleNotFound(); });

    server_.begin();
    started_ = true;
}

void WebPortal::loop() {
    if (started_) {
        server_.handleClient();
    }
}

void WebPortal::pageHead(String &out, const char *title,
                         const char *activePath) const {
    out.reserve(6000);
    out += F("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,"
             "initial-scale=1\"><title>");
    out += title;
    out += F(" &middot; OpenPrintTag dryer</title>");
    out += kStyle;
    out += F("</head><body><header><h1>");
    out += escape(app_->settings().deviceName);
    out += F("</h1><nav>");
    for (const NavItem &item : kNav) {
        out += F("<a href=\"");
        out += item.path;
        out += F("\"");
        if (strcmp(item.path, activePath) == 0) {
            out += F(" class=\"on\"");
        }
        out += F(">");
        out += item.label;
        out += F("</a>");
    }
    out += F("</nav></header><main>");
}

void WebPortal::pageFoot(String &out) const {
    out += F("<p class=\"dim\" style=\"font-size:12.5px\">firmware ");
    out += FW_VERSION;
    out += F(" &middot; ");
    out += deviceId_;
    if (hooks_.networkSummary) {
        out += F(" &middot; ");
        out += escape(hooks_.networkSummary());
    }
    out += F("</p></main></body></html>");
}

void WebPortal::sendPage(const String &body) {
    server_.send(200, "text/html; charset=utf-8", body);
}

// Every page that can be redirected to has to render this, or the result of
// whatever the user just did vanishes silently.
void WebPortal::renderFlash(String &out) {
    if (flash_.length() > 0) {
        out += F("<div class=\"flash\">");
        out += escape(flash_);
        out += F("</div>");
        flash_ = "";
    }
}

// Spoolman, on the Tag page because everything here is about the tag in the
// field: which spool it is, and whether the weight has reached Spoolman.
void WebPortal::renderSpoolmanCard(String &out) {
    const SpoolmanClient::Status &spoolman = app_->spoolman().status();
    const NfcManager::Status &nfc = app_->nfc().status();

    out += F("<div class=\"card\"><h2>Spoolman</h2>");

    if (!spoolman.configured) {
        out += F("<p class=\"dim\">Not configured. If you run Spoolman, put its "
                 "address on the <a href=\"/settings\">Settings</a> page and "
                 "this device will keep the spool's remaining weight honest: a "
                 "measurement replaces the estimate a printer reported.</p>"
                 "</div>");
        return;
    }

    out += F("<table>");
    row(out, "Server", escape(app_->spoolman().baseUrl()));
    row(out, "Automatic updates",
        app_->settings().spoolmanPush ? String("on") : String("off"));
    // Which of the two identification schemes this server turned out to
    // understand. Worth showing: it is the difference between a Spoolman
    // release and its master branch, and it changes what the Spoolman UI shows
    // about a linked tag.
    row(out, "Tags identified by",
        String(SpoolmanClient::apiName(spoolman.api)));
    if (spoolman.linkedSpoolId > 0) {
        row(out, "This tag is", String("spool ") + spoolman.linkedSpoolId);
    } else if (spoolman.lookedUp) {
        row(out, "This tag is", String("not known to Spoolman"));
    } else {
        row(out, "This tag is", String("not looked up yet"));
    }
    if (spoolman.measureCount > 0) {
        row(out, "Last measurement sent",
            grams(spoolman.lastMeasureGrams) + " (" +
                String(spoolman.measureCount) + " in total)");
    }
    if (spoolman.lastError[0] != '\0') {
        row(out, "Last error", escape(spoolman.lastError));
    } else if (app_->spoolmanStatus()[0] != '\0') {
        row(out, "Last result", escape(app_->spoolmanStatus()));
    }
    out += F("</table>");

    if (!nfc.tagPresent) {
        out += F("<p class=\"dim\">Put a tag in the field to link it or send a "
                 "weight.</p></div>");
        return;
    }

    if (spoolman.linkedSpoolId > 0) {
        out += F("<form method=\"post\" action=\"/action\">"
                 "<button name=\"do\" value=\"spoolman_push\" "
                 "class=\"primary\">Send the weight to Spoolman now</button>"
                 "<button name=\"do\" value=\"spoolman_unlink\">Unlink this "
                 "tag</button></form>");
    } else {
        // The one manual step in the whole flow, and only once per spool: the
        // tag carries no Spoolman identity, so somebody has to say which row in
        // the inventory this physical spool is. The number is in Spoolman's URL
        // when the spool is open, and on its label if you print those.
        out += F("<p class=\"dim\">Say which Spoolman spool this tag belongs "
                 "to, once. After that every measurement finds it on its "
                 "own.</p>"
                 "<form method=\"post\" action=\"/action\">"
                 "<input type=\"hidden\" name=\"do\" value=\"spoolman_link\">"
                 "<label for=\"spool_no\">Spoolman spool number</label>"
                 "<input id=\"spool_no\" name=\"spool\" inputmode=\"numeric\" "
                 "placeholder=\"42\">"
                 "<button type=\"submit\" class=\"primary\">Link this "
                 "tag</button></form>");
    }
    out += F("</div>");
}

void WebPortal::redirectWith(const char *path, const String &message) {
    flash_ = message;
    server_.sendHeader("Location", path, true);
    server_.send(303, "text/plain", "see other");
}

void WebPortal::handleRoot() {
    const App::WeightCheck unusedCheck = {};
    (void)unusedCheck;

    const NfcManager::Status &nfc = app_->nfc().status();
    const ScaleManager::Status &scale = app_->scale().status();
    const SpoolState::State &spool = app_->spool();
    const opt::OptMainData &main = app_->tag().main;

    String out;
    pageHead(out, "Status", "/");
    renderFlash(out);

    if (!scale.calibrated) {
        out += F("<div class=\"flash\">The scale has never been calibrated, so "
                 "every weight below is meaningless. Start on the "
                 "<a href=\"/calibrate\">Scale</a> page.</div>");
    }

    // Spool card.
    out += F("<div class=\"card\"><h2>Spool</h2>");
    if (!nfc.tagDecoded) {
        out += F("<p class=\"dim\">");
        out += nfc.tagPresent ? F("A tag is in the field but its contents could "
                                  "not be decoded.")
                              : F("No tag in the field.");
        out += F("</p>");
        if (nfc.lastError[0] != '\0') {
            out += F("<p class=\"bad\">");
            out += escape(nfc.lastError);
            out += F("</p>");
        }
    } else {
        out += F("<div class=\"big\">");
        if (main.hasPrimaryColor) {
            char swatch[32];
            snprintf(swatch, sizeof(swatch), "#%02X%02X%02X",
                     main.primaryColor[0], main.primaryColor[1],
                     main.primaryColor[2]);
            out += F("<span class=\"sw\" style=\"background:");
            out += swatch;
            out += F("\"></span>");
        }
        out += escape(main.materialName);
        out += F("</div>");

        if (spool.referenceWeightsPresent) {
            const int percent = static_cast<int>(spool.remainingPercent + 0.5f);
            out += F("<div class=\"bar\"><i style=\"width:");
            out += percent;
            out += F("%\"></i></div><div class=\"big\">");
            out += String(spool.remainingGrams, 0);
            out += F(" <span>g left of ");
            out += String(spool.referenceFullGrams, 0);
            out += F(" g");
            if (spool.remainingLengthValid) {
                out += F(", ");
                out += String(spool.remainingLengthMm / 1000.0f, 0);
                out += F(" m");
            }
            out += F("</span></div>");
        }

        out += F("<table>");
        row(out, "Brand", escape(main.brandName));
        if (main.hasMaterialType) {
            const char *name = opt::materialTypeName(main.materialType);
            row(out, "Material",
                name != nullptr ? String(name) : String(main.materialType));
        }
        if (main.brandSpecificInstanceId[0] != '\0') {
            row(out, "Spool ID", escape(main.brandSpecificInstanceId));
        }
        if (spool.referenceWeightsPresent) {
            row(out, "Consumed (scale)", grams(spool.consumedGrams, 0));
        }
        if (spool.tagConsumedValid) {
            row(out, "Consumed (on tag)", grams(spool.tagConsumedGrams, 0));
        }
        if (main.hasDryingTemperature) {
            String drying = String(main.dryingTemperature) + " \xC2\xB0" "C";
            if (main.hasDryingTime) {
                drying += " for " + String(main.dryingTime / 60) + " h";
            }
            if (main.dryingTemperature > presets::kDryerMaxTemperatureC) {
                drying += F(" <span class=\"warn\">(hotter than this dryer "
                            "can go)</span>");
            }
            row(out, "Recommended drying", drying);
        }
        if (main.hasMinPrintTemperature && main.hasMaxPrintTemperature) {
            row(out, "Print temperature",
                String(main.minPrintTemperature) + "-" +
                    String(main.maxPrintTemperature) + " \xC2\xB0" "C");
        }
        row(out, "Tag UID", escape(nfc.uid));
        out += F("</table>");

        if (spool.disagrees) {
            out += F("<p class=\"warn\">The scale and the tag disagree by ");
            out += String(spool.disagreementGrams, 0);
            out += F(" g. Either the dryer was moved or the tare has drifted; "
                     "check the tare before trusting the write-back.</p>");
        }
        if (spool.clampedEmpty || spool.clampedFull) {
            out += F("<p class=\"warn\">The measured weight is outside what "
                     "this spool can hold, so it has been clamped. That "
                     "usually means the tare is wrong.</p>");
        }
    }
    out += F("</div>");

    // Weight card.
    out += F("<div class=\"card\"><h2>Scale</h2><table>");
    row(out, "Gross weight",
        scale.haveReading ? grams(scale.grossGrams) : String("no reading"));
    row(out, "Minus dryer tare", grams(app_->scale().spoolGrams()));
    row(out, "Dryer tare", grams(scale.dryerTareGrams));
    row(out, "Stable", scale.stable ? String(F("<span class=\"ok\">yes</span>"))
                                    : String(F("settling")));
    if (scale.temperatureValid) {
        String temperature = String(scale.temperatureC, 1) + " \xC2\xB0" "C";
        if (scale.driftPerC != 0.0f) {
            temperature += " (correcting " +
                           String(scale.driftCorrectionGrams, 1) + " g)";
        } else {
            temperature += F(" <span class=\"dim\">(no drift coefficient "
                             "set)</span>");
        }
        row(out, "Load cell", temperature);
    }
    if (spool.impliedTareValid) {
        row(out, "Tare implied by this sealed spool",
            grams(spool.impliedDryerTareGrams, 0) + " (" +
                String(spool.impliedTareErrorGrams, 1) + " g from stored)");
    }
    out += F("</table>");
    out += F("<form method=\"post\" action=\"/action\">"
             "<button name=\"do\" value=\"write\" class=\"primary\">"
             "Write tag now</button>"
             "<button name=\"do\" value=\"refresh\">Re-read tag</button>"
             "</form></div>");

    // Write-back card.
    out += F("<div class=\"card\"><h2>Write-back</h2><table>");
    row(out, "Automatic",
        app_->policy().enabled() ? String(F("on")) : String(F("off")));
    row(out, "Status", escape(app_->writeStatus()));
    row(out, "Blocks written", String(nfc.blocksWritten));
    row(out, "Workgroup", escape(app_->settings().workgroup));
    out += F("</table><form method=\"post\" action=\"/action\">");
    if (app_->policy().enabled()) {
        out += F("<button name=\"do\" value=\"auto_off\">Turn off</button>");
    } else {
        out += F("<button name=\"do\" value=\"auto_on\">Turn on</button>");
    }
    out += F("<button name=\"do\" value=\"wipe\" class=\"danger\">"
             "Wipe aux region</button></form></div>");

    pageFoot(out);
    sendPage(out);
}

void WebPortal::handleProvision() {
    String out;
    pageHead(out, "New spool", "/provision");
    renderFlash(out);

    out += F("<div class=\"card\"><h2>Provision a blank tag</h2>"
             "<p class=\"dim\">Stick a blank OpenPrintTag on the spool flange, "
             "put the spool in the dryer, and copy the three measured numbers "
             "from the spool's page at prusament.com. The device checks them "
             "against the scale before it writes anything.</p>"
             "<form method=\"post\" action=\"/provision\">"
             "<label for=\"material\">Material</label>"
             "<select id=\"material\" name=\"material\">");

    size_t materialCount = 0;
    const presets::MaterialPreset *materials = presets::all(materialCount);
    for (size_t i = 0; i < materialCount; ++i) {
        out += F("<option value=\"");
        out += materials[i].id;
        out += F("\">");
        out += materials[i].displayName;
        if (presets::needsHotterDryer(materials[i])) {
            out += F(" (wants ");
            out += materials[i].dryingC;
            out += F(" \xC2\xB0" "C to dry)");
        }
        out += F("</option>");
    }
    out += F("</select>");

    out += F("<div class=\"row\"><div><label for=\"color\">Colour</label>"
             "<select id=\"color\" name=\"color\">");
    size_t colorCount = 0;
    const presets::ColorPreset *colors = presets::colors(colorCount);
    for (size_t i = 0; i < colorCount; ++i) {
        out += F("<option value=\"");
        out += escape(colors[i].name);
        out += F("\">");
        out += escape(colors[i].name);
        out += F("</option>");
    }
    out += F("</select></div>"
             "<div><label for=\"color_name\">Or one not in the list</label>"
             "<input id=\"color_name\" name=\"color_name\" "
             "placeholder=\"Linden Light\"></div>"
             "<div><label for=\"hex\">Colour override</label>"
             "<input id=\"hex\" name=\"hex\" placeholder=\"#24292a\"></div>"
             "</div>");

    out += F("<div class=\"row\"><div><label for=\"spool_id\">Spool ID from the "
             "QR code</label><input id=\"spool_id\" name=\"spool_id\" "
             "placeholder=\"7ab2acb509\"></div>"
             "<div><label for=\"length\">Length (m)</label>"
             "<input id=\"length\" name=\"length\" placeholder=\"343\"></div>"
             "</div>");

    // The spool weight is a placeholder rather than a filled-in value on
    // purpose. Prusament spool weights have changed with the generations -- a
    // 2025 spool reads 186 g where an older one reads 201 or 280 -- and the
    // tare cross-check downstream cares about 5 g, so a plausible default that
    // nobody questions is worse than an empty box.
    //
    // The placeholder adds the adapter to that figure because this field is the
    // spool's whole tare, and everything downstream treats it that way: the
    // scale cross-check below, the remaining-filament sums, and the guard that
    // stops a reading heavier than a full spool reaching Spoolman. Leaving the
    // adapter out puts every one of them 31 g off in the same direction.
    out += F("<div class=\"row\"><div><label for=\"filament\">Filament weight "
             "(g)</label><input id=\"filament\" name=\"filament\" "
             "placeholder=\"1044\" required></div>"
             "<div><label for=\"spool\">Spool weight (g): the spool's own page "
             "plus the adapter</label><input id=\"spool\" name=\"spool\" "
             "placeholder=\"");
    out += String(presets::kPrusamentSpoolGrams + OPT_ADAPTER_GRAMS, 0);
    out += F("\" required></div></div>");

    // The live cross-check, so a mistake is visible before anything is written.
    const ScaleManager::Status &scale = app_->scale().status();
    out += F("<p class=\"dim\">Scale right now: ");
    if (!scale.haveReading) {
        out += F("no reading.");
    } else {
        out += String(app_->scale().spoolGrams(), 0);
        out += F(" g on top of the dryer tare");
        out += scale.stable ? F(", stable.") : F(", still settling.");
    }
    out += F("</p>");

    out += F("<label><input type=\"checkbox\" name=\"uri\" value=\"1\"> "
             "Also write a link to the spool's page (costs tag memory)</label>"
             "<label><input type=\"checkbox\" name=\"override\" value=\"1\"> "
             "Write even if the scale disagrees with the entered "
             "weights</label>"
             "<button type=\"submit\" class=\"primary\">Write tag</button>"
             "</form></div>");

    pageFoot(out);
    sendPage(out);
}

void WebPortal::handleProvisionPost() {
    App::ProvisionInput input;
    snprintf(input.materialId, sizeof(input.materialId), "%s",
             server_.arg("material").c_str());
    // A typed-in colour wins over the list. The colour name is not decoration:
    // material_name is "<prefix> <colour>", and material_uuid is derived from
    // that, so a colour Prusa sells but this table has not caught up with still
    // has to be spelled the way Prusa spells it.
    String typedColor = server_.arg("color_name");
    typedColor.trim();
    snprintf(input.colorName, sizeof(input.colorName), "%s",
             typedColor.length() > 0 ? typedColor.c_str()
                                     : server_.arg("color").c_str());
    snprintf(input.colorHex, sizeof(input.colorHex), "%s",
             server_.arg("hex").c_str());
    snprintf(input.spoolId, sizeof(input.spoolId), "%s",
             server_.arg("spool_id").c_str());
    input.filamentGrams = server_.arg("filament").toFloat();
    input.spoolGrams = server_.arg("spool").toFloat();
    input.lengthMetres = server_.arg("length").toFloat();
    input.includeUri = server_.hasArg("uri");
    input.ignoreWeightCheck = server_.hasArg("override");

    // "Custom" is the placeholder in the colour list; without a hex value it
    // would end up as the literal material name "PETG Custom".
    if (strcmp(input.colorName, "Custom") == 0 && input.colorHex[0] == '\0') {
        redirectWith("/provision",
                     String("pick a real colour or enter a hex value"));
        return;
    }

    redirectWith("/provision", app_->provision(input));
}

void WebPortal::handleCalibrate() {
    const ScaleManager::Status &scale = app_->scale().status();

    String out;
    pageHead(out, "Scale", "/calibrate");
    renderFlash(out);

    out += F("<div class=\"card\"><h2>Current calibration</h2><table>");
    row(out, "Raw counts", String(scale.rawCounts));
    row(out, "Zero", String(scale.zeroCounts, 0) + " counts");
    row(out, "Span", String(scale.countsPerGram, 3) + " counts/g" +
                         (scale.calibrated ? "" : " (never calibrated)"));
    row(out, "Dryer tare", grams(scale.dryerTareGrams));
    row(out, "Gross now",
        scale.haveReading ? grams(scale.grossGrams) : String("no reading"));
    row(out, "Drift coefficient", String(scale.driftPerC, 3) + " g/\xC2\xB0" "C");
    if (scale.temperatureValid) {
        row(out, "Load cell temperature",
            String(scale.temperatureC, 1) + " \xC2\xB0" "C");
    }
    row(out, "Sample errors", String(scale.sampleErrorCount));
    out += F("</table></div>");

    out += F("<div class=\"card\"><h2>Calibrate</h2>"
             "<p class=\"dim\">In order: zero the empty platform, put a known "
             "mass on it and calibrate the span, then put the empty dryer on "
             "and tare. Each step waits for the reading to settle.</p>"
             "<form method=\"post\" action=\"/calibrate\">"
             "<button name=\"do\" value=\"zero\">1. Zero empty platform</button>"
             "<label for=\"mass\">2. Known mass on the platform (g)</label>"
             "<input id=\"mass\" name=\"mass\" value=\"");
    out += String(app_->settings().calibrationMassGrams, 0);
    out += F("\"><button name=\"do\" value=\"span\" class=\"primary\">"
             "Calibrate span</button>"
             "<p class=\"dim\">3. Put the empty dryer on the platform with its "
             "lid closed.</p>"
             "<button name=\"do\" value=\"tare\">Tare dryer</button>"
             "</form></div>");

    out += F("<div class=\"card\"><h2>Cross-checks</h2>");
    const SpoolState::State &spool = app_->spool();
    if (spool.impliedTareValid) {
        out += F("<p>This spool's tag says nothing has been consumed yet, so "
                 "the dryer must weigh ");
        out += String(spool.impliedDryerTareGrams, 0);
        out += F(" g. The stored tare is ");
        out += String(spool.impliedTareErrorGrams, 1);
        out += F(" g away from that.</p>"
                 "<form method=\"post\" action=\"/action\">"
                 "<button name=\"do\" value=\"adopt_tare\">Use the implied "
                 "tare</button></form>");
    } else {
        out += F("<p class=\"dim\">With a sealed spool in the dryer (one whose "
                 "tag reports nothing consumed), the tag's own reference "
                 "weights imply what the dryer weighs, which checks the tare "
                 "without needing a known mass.</p>");
    }
    out += F("<form method=\"post\" action=\"/calibrate\">"
             "<label for=\"drift\">Thermal drift coefficient (g per "
             "\xC2\xB0" "C)</label>"
             "<input id=\"drift\" name=\"drift\" value=\"");
    out += String(scale.driftPerC, 3);
    out += F("\"><button name=\"do\" value=\"drift\">Save coefficient</button>"
             "</form></div>");

    pageFoot(out);
    sendPage(out);
}

void WebPortal::handleCalibratePost() {
    const String what = server_.arg("do");
    String result;

    if (what == "zero") {
        char message[128] = {0};
        const bool ok = app_->scale().zeroPlatform(message, sizeof(message));
        result = String(ok ? "" : "could not zero: ") + message;
    } else if (what == "span") {
        const float mass = server_.arg("mass").toFloat();
        result = app_->setCalibrationMass(mass);
        if (result.indexOf("must be") < 0) {
            result = app_->calibrate();
        }
    } else if (what == "tare") {
        result = app_->tareDryer();
    } else if (what == "drift") {
        result = app_->setDriftCoefficient(server_.arg("drift").toFloat());
    } else {
        result = F("unknown calibration step");
    }

    redirectWith("/calibrate", result);
}

void WebPortal::handleSettings() {
    const Settings &settings = app_->settings();

    String out;
    pageHead(out, "Settings", "/settings");
    renderFlash(out);

    out += F("<form method=\"post\" action=\"/settings\">"
             "<div class=\"card\"><h2>Wi-Fi</h2>"
             "<label for=\"ssid\">Network</label>"
             "<input id=\"ssid\" name=\"ssid\" value=\"");
    out += escape(settings.wifiSsid);
    out += F("\"><label for=\"wifi_pass\">Password</label>"
             "<input id=\"wifi_pass\" name=\"wifi_pass\" type=\"password\" "
             "placeholder=\"unchanged\"></div>");

    out += F("<div class=\"card\"><h2>MQTT broker</h2>"
             "<div class=\"row\"><div><label for=\"host\">Host</label>"
             "<input id=\"host\" name=\"host\" value=\"");
    out += escape(settings.mqttHost);
    out += F("\"></div><div><label for=\"port\">Port</label>"
             "<input id=\"port\" name=\"port\" value=\"");
    out += String(settings.mqttPort);
    out += F("\"></div></div>"
             "<div class=\"row\"><div><label for=\"user\">User</label>"
             "<input id=\"user\" name=\"user\" value=\"");
    out += escape(settings.mqttUser);
    out += F("\"></div><div><label for=\"pass\">Password</label>"
             "<input id=\"pass\" name=\"pass\" type=\"password\" "
             "placeholder=\"unchanged\"></div></div></div>");

    out += F("<div class=\"card\"><h2>Spoolman</h2>"
             "<p class=\"dim\">Where Spoolman is, if you run it. Plain http "
             "only, port 7912 by default. Leave it empty to switch the whole "
             "thing off. The dryer looks a tag up by its UID and reports the "
             "weight it measures, which is the one number Spoolman otherwise "
             "only ever gets as an estimate.</p>"
             "<label for=\"spoolman\">Spoolman address</label>"
             "<input id=\"spoolman\" name=\"spoolman\" "
             "placeholder=\"http://192.168.1.10:7912\" value=\"");
    out += escape(settings.spoolmanUrl);
    out += F("\"><label><input type=\"checkbox\" name=\"spoolman_push\" "
             "value=\"1\"");
    if (settings.spoolmanPush) {
        out += F(" checked");
    }
    out += F("> Send measurements automatically</label>"
             "<p class=\"dim\">With this off, nothing is sent until you press "
             "the button on the Tag page.</p></div>");

    out += F("<div class=\"card\"><h2>Tag writing</h2>"
             "<label for=\"workgroup\">Workgroup (up to 8 characters, shared "
             "between devices that trust each other's aux data)</label>"
             "<input id=\"workgroup\" name=\"workgroup\" maxlength=\"8\" "
             "value=\"");
    out += escape(settings.workgroup);
    out += F("\"><label for=\"storage\">Storage location written to tags</label>"
             "<input id=\"storage\" name=\"storage\" maxlength=\"8\" value=\"");
    out += escape(settings.storageLocation);
    out += F("\"><div class=\"row\"><div>"
             "<label for=\"delta\">Minimum change before writing (g)</label>"
             "<input id=\"delta\" name=\"delta\" value=\"");
    out += String(settings.writeMinDeltaGrams, 1);
    out += F("\"></div><div><label for=\"interval\">Minimum interval between "
             "writes (minutes)</label><input id=\"interval\" name=\"interval\" "
             "value=\"");
    out += String(settings.writeMinIntervalMinutes);
    out += F("\"></div></div>"
             "<label for=\"name\">Device name</label>"
             "<input id=\"name\" name=\"name\" value=\"");
    out += escape(settings.deviceName);
    out += F("\"><button type=\"submit\" class=\"primary\">Save</button>"
             "</div></form>");

    out += F("<div class=\"card\"><h2>Maintenance</h2>"
             "<p class=\"dim\">A firmware image can also be pushed from the "
             "Arduino IDE over the network once the device is on Wi-Fi.</p>"
             "<p><a href=\"/update\">Upload firmware</a> &middot; "
             "<a href=\"/api/state\">State JSON</a></p>"
             "<form method=\"post\" action=\"/action\">"
             "<button name=\"do\" value=\"restart\">Restart</button>"
             "</form></div>");

    pageFoot(out);
    sendPage(out);
}

void WebPortal::handleSettingsPost() {
    Settings &settings = app_->settings();

    const String ssid = server_.arg("ssid");
    const String wifiPassword = server_.arg("wifi_pass");
    const bool wifiChanged =
        ssid != String(settings.wifiSsid) || wifiPassword.length() > 0;

    snprintf(settings.wifiSsid, sizeof(settings.wifiSsid), "%s", ssid.c_str());
    // An empty password field means "leave it alone", so a settings save does
    // not lock the device out of its own network.
    if (wifiPassword.length() > 0) {
        snprintf(settings.wifiPassword, sizeof(settings.wifiPassword), "%s",
                 wifiPassword.c_str());
    }

    const String host = server_.arg("host");
    const String mqttPassword = server_.arg("pass");
    const bool mqttChanged = host != String(settings.mqttHost) ||
                             mqttPassword.length() > 0 ||
                             server_.arg("user") != String(settings.mqttUser) ||
                             server_.arg("port").toInt() != settings.mqttPort;
    snprintf(settings.mqttHost, sizeof(settings.mqttHost), "%s", host.c_str());
    if (server_.hasArg("port")) {
        const long port = server_.arg("port").toInt();
        settings.mqttPort = (port > 0 && port < 65536)
                                ? static_cast<uint16_t>(port)
                                : 1883;
    }
    snprintf(settings.mqttUser, sizeof(settings.mqttUser), "%s",
             server_.arg("user").c_str());
    if (mqttPassword.length() > 0) {
        snprintf(settings.mqttPassword, sizeof(settings.mqttPassword), "%s",
                 mqttPassword.c_str());
    }

    if (server_.hasArg("spoolman")) {
        String url = server_.arg("spoolman");
        url.trim();
        // A host on its own is the likeliest thing to be typed, and rejecting
        // it would be pedantry when the default port is known.
        if (url.length() > 0 && !url.startsWith("http://") &&
            !url.startsWith("https://")) {
            url = String("http://") + url;
        }
        snprintf(settings.spoolmanUrl, sizeof(settings.spoolmanUrl), "%s",
                 url.c_str());
    }
    // An unchecked box is simply absent from the form, which is what makes this
    // the switch rather than a value to read.
    settings.spoolmanPush = server_.hasArg("spoolman_push");

    if (server_.hasArg("workgroup")) {
        snprintf(settings.workgroup, sizeof(settings.workgroup), "%s",
                 server_.arg("workgroup").c_str());
    }
    if (server_.hasArg("storage")) {
        snprintf(settings.storageLocation, sizeof(settings.storageLocation),
                 "%s", server_.arg("storage").c_str());
    }
    if (server_.hasArg("name") && server_.arg("name").length() > 0) {
        snprintf(settings.deviceName, sizeof(settings.deviceName), "%s",
                 server_.arg("name").c_str());
    }
    if (server_.hasArg("delta")) {
        const float delta = server_.arg("delta").toFloat();
        if (delta >= 1.0f && delta <= 500.0f) {
            settings.writeMinDeltaGrams = delta;
        }
    }
    if (server_.hasArg("interval")) {
        const long minutes = server_.arg("interval").toInt();
        if (minutes >= 0 && minutes <= 1440) {
            settings.writeMinIntervalMinutes = static_cast<uint32_t>(minutes);
        }
    }

    settings.save();
    app_->applySpoolmanSettings();

    String result = F("settings saved");
    if (wifiChanged && hooks_.reconnectWifi) {
        hooks_.reconnectWifi();
        result += F(", reconnecting to Wi-Fi");
    }
    if (mqttChanged && hooks_.reconnectMqtt) {
        hooks_.reconnectMqtt();
        result += F(", reconnecting to the broker");
    }
    redirectWith("/settings", result);
}

void WebPortal::handleTagDump() {
    const NfcManager::Status &nfc = app_->nfc().status();
    const opt::OptTag &tag = app_->tag();

    String out;
    pageHead(out, "Tag", "/tag");
    renderFlash(out);
    out += F("<div class=\"card\"><h2>Reader</h2><table>");
    row(out, "Reader",
        nfc.readerOnline ? escape(nfc.readerVersion) : String("offline"));
    row(out, "Multi-block reads",
        nfc.multiBlockReads ? String("yes") : String("no, one block at a time"));
    row(out, "Reads / errors",
        String(nfc.readCount) + " / " + String(nfc.readErrorCount));
    row(out, "Writes / errors",
        String(nfc.writeCount) + " / " + String(nfc.writeErrorCount));
    if (nfc.lastError[0] != '\0') {
        row(out, "Last error", escape(nfc.lastError));
    }
    out += F("</table></div>");

    if (!nfc.tagPresent) {
        out += F("<div class=\"card\"><p class=\"dim\">No tag in the field."
                 "</p></div>");
        // Still worth showing: it says whether Spoolman is configured at all,
        // and carries the result of the last push, which is the thing you come
        // to this page to check after one has failed.
        renderSpoolmanCard(out);
        pageFoot(out);
        sendPage(out);
        return;
    }

    out += F("<div class=\"card\"><h2>Layout</h2><table>");
    row(out, "UID", escape(nfc.uid));
    row(out, "Geometry", String(nfc.blockCount) + " blocks of " +
                             String(nfc.blockSize) + " B = " +
                             String(nfc.imageBytes) + " B");
    row(out, "Decode",
        nfc.tagDecoded ? String(F("<span class=\"ok\">OpenPrintTag</span>"))
                       : String(opt::optStatusText(nfc.decodeStatus)));
    if (nfc.tagDecoded) {
        row(out, "Payload",
            String(tag.regions.payloadSize) + " B at offset " +
                String(tag.regions.payloadStart));
        row(out, "Main region", String(tag.regions.mainSize) + " B at " +
                                    String(tag.regions.mainAbsolute()));
        if (tag.regions.hasAux) {
            row(out, "Aux region", String(tag.regions.auxSize) + " B at " +
                                       String(tag.regions.auxAbsolute()));
        } else {
            row(out, "Aux region", String(F("none, so nothing can be written")));
        }
        row(out, "Identity UUIDs",
            String(tag.main.hasInstanceUuid || tag.main.hasBrandUuid
                       ? F("stored on the tag")
                       : F("derived from the fields, not stored")));
        row(out, "Capability container",
            String(tag.regions.ccCapacity) + " B declared");
        row(out, "Unknown fields carried over",
            String(tag.main.unknownFieldCount) + " main, " +
                String(tag.aux.unknownFieldCount) + " aux");
        if (tag.auxStatus != opt::OptStatus::Ok) {
            row(out, "Aux region problem",
                String(opt::optStatusText(tag.auxStatus)));
        }
    }
    out += F("</table>");

    // A tag with no auxiliary region reads perfectly and can never be written
    // to, which is a confusing state to be left in. The offer to fix it belongs
    // next to the diagnosis, and it copies the main region rather than
    // rebuilding it from a preset, so nothing on a factory tag is reinterpreted.
    if (nfc.tagDecoded && !tag.regions.hasAux) {
        out += F("<form method=\"post\" action=\"/action\">"
                 "<input type=\"hidden\" name=\"do\" value=\"add_aux\">"
                 "<p class=\"dim\">This tag can be read but never written: "
                 "there is no auxiliary region for consumed weight to go in. "
                 "Rewriting it keeps the main region byte for byte and only "
                 "changes the layout, so every field stays as its writer left "
                 "it.</p>"
                 "<button type=\"submit\">Add an auxiliary region</button>"
                 "</form>");
    }

    // Correcting the tare in place, rather than through the provisioning form.
    // Re-provisioning would put a portal-shaped tag over a manufacturer's one and
    // lose the GTIN, the manufactured date, the container geometry and the rest,
    // none of which the form can ask for. This changes the one field and leaves
    // every other byte of meaning alone.
    if (nfc.tagDecoded && tag.regions.hasAux) {
        out += F("<form method=\"post\" action=\"/action\">"
                 "<input type=\"hidden\" name=\"do\" value=\"empty_weight\">"
                 "<p class=\"dim\">The empty spool weight is the tare: "
                 "everything still on the platform once the filament is gone. "
                 "Correct it here if the spool has gained something permanent "
                 "since it was written, such as a bonded adapter, rather than "
                 "re-provisioning, which would overwrite the manufacturer's "
                 "fields with what this form can ask for.</p>"
                 "<label for=\"empty\">Empty spool weight (g)</label>"
                 "<input id=\"empty\" name=\"empty\" placeholder=\"");
        if (tag.main.hasEmptyContainerWeight) {
            out += String(tag.main.emptyContainerWeight +
                              OPT_ADAPTER_GRAMS, 0);
        } else {
            out += String(presets::kPrusamentSpoolGrams + OPT_ADAPTER_GRAMS, 0);
        }
        out += F("\">"
                 "<button type=\"submit\">Rewrite the empty spool weight"
                 "</button></form>");
    }
    out += F("</div>");

    renderSpoolmanCard(out);

    // Raw bytes, with the region boundaries marked. This is the page that
    // settles arguments about what is actually on a tag.
    out += F("<div class=\"card\"><h2>Raw memory</h2><pre>");
    const uint8_t *image = app_->nfc().image();
    const size_t length = app_->nfc().imageLength();
    const size_t mainStart = tag.regions.mainAbsolute();
    const size_t mainEnd = mainStart + tag.regions.mainSize;
    const size_t auxStart = tag.regions.auxAbsolute();
    const size_t auxEnd = auxStart + tag.regions.auxSize;

    for (size_t offset = 0; offset < length; offset += 16) {
        char line[16];
        snprintf(line, sizeof(line), "%03u  ", static_cast<unsigned>(offset));
        out += line;
        for (size_t i = 0; i < 16; ++i) {
            if (offset + i >= length) {
                out += F("   ");
                continue;
            }
            snprintf(line, sizeof(line), "%02X ", image[offset + i]);
            out += line;
        }
        out += F(" ");
        for (size_t i = 0; i < 16 && offset + i < length; ++i) {
            const uint8_t byte = image[offset + i];
            out += (byte >= 0x20 && byte < 0x7F)
                       ? escape(String(static_cast<char>(byte)))
                       : String('.');
        }
        // Mark which region this line belongs to.
        if (nfc.tagDecoded) {
            const size_t lineEnd = offset + 16;
            if (offset < mainEnd && lineEnd > mainStart) {
                out += F("  main");
            }
            if (tag.regions.hasAux && offset < auxEnd && lineEnd > auxStart) {
                out += F("  aux");
            }
        }
        out += '\n';
    }
    out += F("</pre><p><a href=\"/tag.bin\">Download these bytes</a> "
             "<span class=\"dim\">as a .bin, which is the format the desktop "
             "test harness reads. A tag written by somebody else's tool is "
             "worth keeping as a fixture.</span></p></div>");

    pageFoot(out);
    sendPage(out);
}

void WebPortal::handleTagBin() {
    const size_t length = app_->nfc().imageLength();
    if (length == 0) {
        server_.send(404, "text/plain", "no tag has been read");
        return;
    }

    // Named after the UID, because the whole point of keeping one of these is
    // being able to tell later which spool it came from. The UID is printed
    // with separators, which do not belong in a filename.
    char filename[48];
    snprintf(filename, sizeof(filename), "tag_%s.bin",
             app_->nfc().status().uid);
    for (char *p = filename; *p != '\0'; ++p) {
        if (*p == ':' || *p == ' ') {
            *p = '_';
        }
    }

    char disposition[80];
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"%s\"", filename);
    server_.sendHeader("Content-Disposition", disposition);
    server_.setContentLength(length);
    server_.send(200, "application/octet-stream", "");
    server_.sendContent(
        reinterpret_cast<const char *>(app_->nfc().image()), length);
}

void WebPortal::handleSelfTest() { renderSelfTest(String()); }

void WebPortal::handleReadTest() {
    const long requested = server_.arg("attempts").toInt();
    const uint16_t attempts =
        requested > 0 ? static_cast<uint16_t>(requested) : 20;
    renderSelfTest(app_->readTest(attempts));
}

void WebPortal::renderSelfTest(const String &readTestReport) {
    const DriftLog &log = app_->driftLog();
    const DriftLog::Fit fit = log.fit();

    String out;
    pageHead(out, "Self-test", "/selftest");
    renderFlash(out);

    out += F("<div class=\"card\"><h2>Self-test</h2><pre>");
    out += escape(app_->selfTest());
    out += F("</pre></div>");

    // Read reliability, which is what decides where the bracket goes.
    out += F("<div class=\"card\"><h2>Read test</h2>"
             "<p class=\"dim\">Put a provisioned spool in the dryer, close the "
             "lid, and run this. Anything below every attempt finding the tag "
             "means the coil wants moving: get it concentric with the spool "
             "first, then closer.</p>");
    if (readTestReport.length() > 0) {
        out += F("<pre>");
        out += escape(readTestReport);
        out += F("</pre>");
    }
    out += F("<form method=\"post\" action=\"/selftest\">"
             "<label for=\"attempts\">Attempts</label>"
             "<input id=\"attempts\" name=\"attempts\" value=\"20\">"
             "<button type=\"submit\" class=\"primary\">Run read test</button>"
             "</form></div>");

    // Thermal drift characterisation.
    out += F("<div class=\"card\"><h2>Drift log</h2>"
             "<p class=\"dim\">Leave a known load on the platform, start the "
             "log, and run a full drying cycle. The fit below turns what the "
             "reading did as the cell warmed into the compensation "
             "coefficient.</p><table>");
    row(out, "State", log.running() ? String(F("<span class=\"ok\">running"
                                               "</span>"))
                                    : String(F("stopped")));
    row(out, "Samples",
        String(log.count()) + " of " + String(log.capacity()) + " every " +
            String(log.intervalSeconds()) + " s");
    if (fit.valid) {
        String verdict = String(fit.gramsPerC, 3) + " g/C over " +
                         String(fit.temperatureSpanC, 1) + " \xC2\xB0" "C, R" +
                         F("<sup>2</sup> ") + String(fit.rSquared, 3);
        if (fit.spanTooNarrow) {
            verdict += F(" <span class=\"warn\">(too narrow a range to "
                         "trust)</span>");
        } else if (fit.rSquared < 0.5f) {
            verdict += F(" <span class=\"warn\">(the weight moved for some "
                         "other reason)</span>");
        }
        row(out, "Fit", verdict);
        row(out, "Weight moved by", grams(fit.weightSpanGrams, 1));
    } else {
        row(out, "Fit", String(F("not enough data yet")));
    }
    const ScaleManager::Status &scale = app_->scale().status();
    row(out, "In use now",
        String(scale.driftPerC, 3) + " g/C referenced to " +
            String(scale.referenceC, 1) + " \xC2\xB0" "C");
    out += F("</table><form method=\"post\" action=\"/action\">"
             "<label for=\"interval\">Sample interval (s)</label>"
             "<input id=\"interval\" name=\"interval\" value=\"");
    out += DRIFT_LOG_DEFAULT_INTERVAL_S;
    out += F("\">"
             "<button name=\"do\" value=\"drift_start\" class=\"primary\">"
             "Start</button>"
             "<button name=\"do\" value=\"drift_stop\">Stop</button>"
             "<button name=\"do\" value=\"drift_apply\">Apply fit</button>"
             "<button name=\"do\" value=\"drift_clear\" class=\"danger\">"
             "Clear</button></form>");
    if (log.count() > 0) {
        out += F("<p><a href=\"/drift.csv\">Download the log as CSV</a> "
                 "<span class=\"dim\">for tools/fit_drift.py</span></p>");
    }
    out += F("</div>");

    pageFoot(out);
    sendPage(out);
}

void WebPortal::handleDriftCsv() {
    // Sent in chunks: a full log is tens of kilobytes, and there is no reason
    // for all of it to exist as a String while the socket copies it out.
    server_.sendHeader("Content-Disposition",
                       "attachment; filename=\"drift.csv\"");
    server_.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server_.send(200, "text/csv; charset=utf-8", "");
    app_->driftLog().writeCsv(
        [this](const String &chunk) { server_.sendContent(chunk); });
    server_.sendContent("");
}

void WebPortal::handleAction() {
    const String what = server_.arg("do");
    String result;

    if (what == "write") {
        result = app_->writeNow();
    } else if (what == "refresh") {
        result = app_->refreshTag();
    } else if (what == "wipe") {
        result = app_->wipeAux();
    } else if (what == "add_aux") {
        result = app_->addAuxRegion();
    } else if (what == "empty_weight") {
        // An empty box would otherwise arrive as 0 g and be refused with a
        // message about plausibility, which describes the wrong mistake.
        String entered = server_.arg("empty");
        entered.trim();
        if (entered.length() == 0) {
            result = String(F("enter the empty spool weight first"));
        } else {
            result = app_->setEmptySpoolWeight(entered.toFloat());
        }
    } else if (what == "adopt_tare") {
        result = app_->adoptImpliedTare();
    } else if (what == "auto_on") {
        result = app_->setAutoWriteBack(true);
    } else if (what == "auto_off") {
        result = app_->setAutoWriteBack(false);
    } else if (what == "poll_on") {
        result = app_->setPollEnabled(true);
    } else if (what == "poll_off") {
        result = app_->setPollEnabled(false);
    } else if (what == "drift_start") {
        const long interval = server_.arg("interval").toInt();
        result = app_->startDriftLog(interval > 0
                                         ? static_cast<uint32_t>(interval)
                                         : DRIFT_LOG_DEFAULT_INTERVAL_S);
    } else if (what == "drift_stop") {
        result = app_->stopDriftLog();
    } else if (what == "drift_clear") {
        result = app_->clearDriftLog();
    } else if (what == "drift_apply") {
        result = app_->applyDriftFit();
    } else if (what == "spoolman_link") {
        result = app_->linkSpoolman(server_.arg("spool").toInt());
    } else if (what == "spoolman_unlink") {
        result = app_->unlinkSpoolman();
    } else if (what == "spoolman_push") {
        result = app_->pushSpoolman();
    } else if (what == "restart") {
        redirectWith("/", String(F("restarting")));
        if (hooks_.restart) {
            hooks_.restart();
        }
        return;
    } else {
        result = F("unknown action");
    }

    const char *back = "/";
    if (what == "adopt_tare") {
        back = "/calibrate";
    } else if (what.startsWith("drift_")) {
        back = "/selftest";
    } else if (what == "add_aux" || what == "empty_weight" ||
               what.startsWith("spoolman_")) {
        back = "/tag";
    }
    redirectWith(back, result);
}

void WebPortal::handleStateJson() {
    if (hooks_.stateJson) {
        server_.send(200, "application/json", hooks_.stateJson());
    } else {
        server_.send(503, "application/json", "{}");
    }
}

void WebPortal::handleUpdatePage() {
    String out;
    pageHead(out, "Firmware", "/settings");
    out += F("<div class=\"card\"><h2>Upload firmware</h2>"
             "<p class=\"dim\">Export a compiled binary from the Arduino IDE "
             "(Sketch &rarr; Export Compiled Binary) and upload it here. The "
             "device restarts when the image has been verified.</p>"
             "<form method=\"post\" action=\"/update\" "
             "enctype=\"multipart/form-data\">"
             "<input type=\"file\" name=\"firmware\" accept=\".bin\">"
             "<button type=\"submit\" class=\"primary\">Upload</button>"
             "</form></div>");
    pageFoot(out);
    sendPage(out);
}

void WebPortal::handleUpdateUpload() {
    HTTPUpload &upload = server_.upload();
    if (upload.status == UPLOAD_FILE_START) {
        // Polling the reader during a flash write is asking for a corrupt
        // image, so the radio stops first.
        app_->nfc().setPollEnabled(false);
        Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        Update.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
        Update.end(true);
    } else {
        Update.abort();
        app_->nfc().setPollEnabled(true);
    }
}

void WebPortal::handleUpdateDone() {
    if (Update.hasError()) {
        app_->nfc().setPollEnabled(true);
        redirectWith("/settings",
                     String(F("firmware upload failed: ")) +
                         Update.errorString());
        return;
    }
    server_.send(200, "text/html; charset=utf-8",
                 String(F("<!doctype html><meta http-equiv=\"refresh\" "
                          "content=\"12;url=/\">Firmware written. Restarting.")));
    if (hooks_.restart) {
        hooks_.restart();
    }
}

void WebPortal::handleNotFound() {
    server_.sendHeader("Location", "/", true);
    server_.send(303, "text/plain", "see other");
}
