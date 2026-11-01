// Spoolman integration.
//
// Spoolman keeps the inventory; this device owns the scale. Its remaining
// weights normally come from a printer's estimate of what a print consumed, so
// they drift from reality over a spool's life, and the whole point of hanging a
// load cell under the dryer is to be able to correct them against a measurement.
//
// Reporting a weight is one call, and the same one in every Spoolman:
//
//   PUT /api/v1/spool/{id}/measure   gross weight of the spool
//
// "Gross weight of the spool" means filament plus the empty spool, and not the
// dryer, which is exactly what this device has after subtracting the dryer tare.
//
// Finding out *which* spool is in the dryer is the part that depends on the
// version, so both ways are implemented and the first scan works out which one
// this server understands:
//
//   Tags        POST /api/v1/tag/scan       -> matched_spool_id
//               PUT  /api/v1/spool/{id}/tag
//     A tag table keyed on the hardware UID, with a uniqueness guarantee and
//     `openprinttag` as a known format. Only in Spoolman's master branch as of
//     v0.26.1, so it cannot be relied on yet.
//
//   ExtraField  GET   /api/v1/spool?extra.nfc_uid=UID
//               PATCH /api/v1/spool/{id}    {"extra":{"nfc_uid":"\"UID\""}}
//     The convention other scanners settled on before the tag table existed,
//     and what every released Spoolman supports. Two things make it sharper
//     than it looks: an extra field that has not been registered makes Spoolman
//     ignore the filter and return the whole inventory, so a match is only
//     believed when the answer actually carries the UID that was asked for, and
//     an answer with more than one spool in it is refused rather than guessed
//     at.
//
// Spoolman has no authentication of its own -- the project's advice is to put a
// reverse proxy in front of it if that matters -- so plain HTTP on the local
// network is what this speaks. HTTPS would need a certificate store the device
// has no way to maintain.
//
// Every call blocks the main loop for up to the configured timeout, so they are
// made on events (a tag appearing, a measurement settling, a button) and never
// on a poll.

#pragma once

#include <Arduino.h>

#include "../config.h"

class SpoolmanClient {
public:
    // Which of the two ways of identifying a tag this server understands.
    enum class Api : uint8_t {
        Unknown,     // no scan has been answered yet
        Tags,        // the tag table, in master
        ExtraField,  // extra.nfc_uid, in every release
    };

    struct Status {
        bool configured = false;
        bool reachable = false;        // last request got an HTTP response
        int lastHttpCode = 0;
        char lastError[96] = {0};      // empty when the last call worked

        // The spool the tag in the field resolves to, or 0 when Spoolman does
        // not know this tag. Cleared when the tag leaves.
        int linkedSpoolId = 0;
        bool lookedUp = false;         // a scan has been answered for this tag

        uint32_t measureCount = 0;
        uint32_t errorCount = 0;
        float lastMeasureGrams = 0.0f;
        uint32_t lastMeasureMs = 0;

        Api api = Api::Unknown;
    };

    // For the portal, which is where somebody would go to find out why their
    // Spoolman behaves differently from the documentation.
    static const char *apiName(Api api);

    void begin(const char *baseUrl, bool enabled, const char *readerName);

    // Re-reads configuration after the settings page changes it.
    void configure(const char *baseUrl, bool enabled, const char *readerName);

    bool enabled() const { return enabled_ && configured(); }
    bool configured() const { return baseUrl_[0] != '\0'; }
    const Status &status() const { return status_; }
    const char *baseUrl() const { return baseUrl_; }

    // Reports a tag scan and remembers which spool it resolved to. Returns
    // false on a transport or protocol failure; a tag Spoolman does not know is
    // a success with `linkedSpoolId` left at zero.
    bool scan(const char *uid, String &detail);

    // Sends a measured gross weight for the spool this tag resolved to.
    bool measure(float grossGrams, String &detail);

    // Links or unlinks the tag in the field to a spool by number, for the first
    // time a spool passes through the dryer.
    bool link(int spoolId, const char *uid, String &detail);
    bool unlink(int spoolId, const char *uid, String &detail);

    // Forgets the resolved spool, for when the tag leaves the field.
    void forgetTag();

private:
    // Performs a request and returns the body. `method` is "GET", "POST",
    // "PUT", "PATCH" or "DELETE"; an empty body sends none.
    bool request(const char *method, const String &path, const String &body,
                 String &response, String &detail);

    // The two lookups, and the two ways of writing a link.
    bool scanViaTagApi(const char *normalizedUid, bool &unsupported,
                       String &detail);
    bool scanViaExtraField(const char *normalizedUid, String &detail);
    bool linkViaTagApi(int spoolId, const char *normalizedUid, bool &unsupported,
                       String &detail);
    bool linkViaExtraField(int spoolId, const char *normalizedUid,
                           String &detail);

    // Registers the nfc_uid field, so that filtering on it actually filters.
    // Harmless to repeat: Spoolman's own endpoint is an upsert.
    bool ensureExtraField(String &detail);

    void noteError(const char *detail);

    char baseUrl_[80] = {0};
    char readerName_[32] = {0};
    bool enabled_ = false;
    bool extraFieldRegistered_ = false;
    Status status_;
};

// The canonical UID spelling: uppercase hex with no separators, which is what
// Spoolman's own normaliser produces. Writing links in that form means a
// Spoolman upgraded to the tag table later finds the same tags.
void spoolmanNormalizeUid(const char *uid, char *out, size_t outSize);

// Pulls one value out of a flat JSON object. Public because they are worth
// testing on their own: this is a deliberately small substitute for a JSON
// parser, and the only thing standing between a surprising response and a wrong
// number acted upon.
namespace spoolman_json {

// Finds `"key": <number>` and returns it. `wasNull` distinguishes an absent key
// from an explicit null, which Spoolman uses for "matched nothing".
bool number(const String &body, const char *key, long &out, bool &wasNull);

// The same, but only for a key at one nesting level. A spool's own `id` in a
// list response is at depth 2; the `id` of the filament nested inside it is
// deeper, and reading that one by accident would attach a weight to the wrong
// spool.
bool numberAtDepth(const String &body, const char *key, int depth, long &out,
                   bool &wasNull);

// How many objects a top-level JSON array holds. Anything other than one is a
// reason not to act.
int arrayLength(const String &body);

// Finds `"key": "<text>"`, unescaping nothing: every value this needs to read
// is hex, a number or a fixed word.
bool text(const String &body, const char *key, char *out, size_t outSize);

// The `message` field of Spoolman's error responses.
String message(const String &body);

}  // namespace spoolman_json
