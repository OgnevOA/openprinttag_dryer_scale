#include "SpoolmanClient.h"

#include <HTTPClient.h>
#include <WiFiClient.h>

#include <string.h>

namespace {

// Skips spaces, tabs and newlines from `pos`.
size_t skipSpace(const String &body, size_t pos) {
    while (pos < body.length()) {
        const char c = body[pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        ++pos;
    }
    return pos;
}

// Finds `"key":` and returns the offset of the value, or -1. `wantDepth` of -1
// accepts the key at any nesting level.
//
// Depth counts containers: the members of a top-level object are at depth 1, and
// the members of an object inside a top-level array are at depth 2.
int findValue(const String &body, const char *key, int wantDepth = -1) {
    const size_t keyLength = strlen(key);
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t stringStart = 0;

    for (size_t i = 0; i < body.length(); ++i) {
        const char c = body[i];

        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
                // A string is a key only when a colon follows it.
                const size_t after = skipSpace(body, i + 1);
                if (after < body.length() && body[after] == ':' &&
                    (wantDepth < 0 || depth == wantDepth) &&
                    i - stringStart == keyLength &&
                    strncmp(body.c_str() + stringStart, key, keyLength) == 0) {
                    return static_cast<int>(skipSpace(body, after + 1));
                }
            }
            continue;
        }

        switch (c) {
            case '"':
                inString = true;
                escaped = false;
                stringStart = i + 1;
                break;
            case '{':
            case '[':
                ++depth;
                break;
            case '}':
            case ']':
                --depth;
                break;
            default:
                break;
        }
    }
    return -1;
}

}  // namespace

namespace spoolman_json {

namespace {

bool numberAt(const String &body, const char *key, int depth, long &out,
              bool &wasNull);

}  // namespace

bool number(const String &body, const char *key, long &out, bool &wasNull) {
    return numberAt(body, key, -1, out, wasNull);
}

bool numberAtDepth(const String &body, const char *key, int depth, long &out,
                   bool &wasNull) {
    return numberAt(body, key, depth, out, wasNull);
}

namespace {

bool numberAt(const String &body, const char *key, int depth, long &out,
              bool &wasNull) {
    wasNull = false;
    const int at = findValue(body, key, depth);
    if (at < 0) {
        return false;
    }
    if (body.startsWith("null", at)) {
        wasNull = true;
        return true;
    }
    size_t pos = at;
    bool negative = false;
    if (pos < body.length() && (body[pos] == '-' || body[pos] == '+')) {
        negative = body[pos] == '-';
        ++pos;
    }
    if (pos >= body.length() || body[pos] < '0' || body[pos] > '9') {
        return false;
    }
    long value = 0;
    while (pos < body.length() && body[pos] >= '0' && body[pos] <= '9') {
        value = value * 10 + (body[pos] - '0');
        ++pos;
    }
    out = negative ? -value : value;
    return true;
}

}  // namespace

int arrayLength(const String &body) {
    int depth = 0;
    int elements = 0;
    bool inString = false;
    bool escaped = false;

    for (size_t i = 0; i < body.length(); ++i) {
        const char c = body[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            continue;
        }
        if (c == '[' || c == '{') {
            ++depth;
            // An object or array that opens directly inside the outer array.
            if (depth == 2) {
                ++elements;
            }
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return elements;
}

bool text(const String &body, const char *key, char *out, size_t outSize) {
    if (outSize == 0) {
        return false;
    }
    out[0] = '\0';
    const int at = findValue(body, key);
    if (at < 0 || static_cast<size_t>(at) >= body.length() || body[at] != '"') {
        return false;
    }
    size_t pos = at + 1;
    size_t written = 0;
    while (pos < body.length() && body[pos] != '"') {
        if (body[pos] == '\\' && pos + 1 < body.length()) {
            ++pos;
        }
        if (written + 1 < outSize) {
            out[written++] = body[pos];
        }
        ++pos;
    }
    out[written] = '\0';
    return true;
}

String message(const String &body) {
    char text_[160] = {0};
    if (text(body, "message", text_, sizeof(text_)) && text_[0] != '\0') {
        return String(text_);
    }
    // Pydantic validation failures come back as a `detail` array instead.
    if (text(body, "msg", text_, sizeof(text_)) && text_[0] != '\0') {
        return String(text_);
    }
    return String();
}

}  // namespace spoolman_json

void SpoolmanClient::begin(const char *baseUrl, bool enabled,
                           const char *readerName) {
    configure(baseUrl, enabled, readerName);
}

void SpoolmanClient::configure(const char *baseUrl, bool enabled,
                               const char *readerName) {
    snprintf(baseUrl_, sizeof(baseUrl_), "%s", baseUrl == nullptr ? "" : baseUrl);

    // Trailing slashes are the most likely thing to be pasted in, and every
    // path below starts with one.
    size_t length = strlen(baseUrl_);
    while (length > 0 && baseUrl_[length - 1] == '/') {
        baseUrl_[--length] = '\0';
    }

    snprintf(readerName_, sizeof(readerName_), "%s",
             readerName == nullptr ? "" : readerName);
    enabled_ = enabled;
    status_.configured = configured();
    // A different server may be a different version, and may not have been told
    // about the nfc_uid field.
    status_.api = Api::Unknown;
    extraFieldRegistered_ = false;
    forgetTag();
}

void SpoolmanClient::forgetTag() {
    status_.linkedSpoolId = 0;
    status_.lookedUp = false;
}

void SpoolmanClient::noteError(const char *detail) {
    snprintf(status_.lastError, sizeof(status_.lastError), "%s", detail);
    ++status_.errorCount;
}

bool SpoolmanClient::request(const char *method, const String &path,
                             const String &body, String &response,
                             String &detail) {
    response = "";
    if (!configured()) {
        detail = F("no Spoolman URL is set");
        noteError(detail.c_str());
        return false;
    }
    if (strncmp(baseUrl_, "http://", 7) != 0) {
        detail = F("the Spoolman URL must start with http://");
        noteError(detail.c_str());
        return false;
    }

    WiFiClient client;
    HTTPClient http;
    client.setTimeout(SPOOLMAN_TIMEOUT_MS / 1000);
    http.setTimeout(SPOOLMAN_TIMEOUT_MS);
    http.setConnectTimeout(SPOOLMAN_TIMEOUT_MS);
    http.setReuse(false);

    const String url = String(baseUrl_) + path;
    if (!http.begin(client, url)) {
        detail = F("could not parse the Spoolman URL");
        noteError(detail.c_str());
        return false;
    }
    if (body.length() > 0) {
        http.addHeader(F("Content-Type"), F("application/json"));
    }

    const int code = http.sendRequest(method, (uint8_t *)body.c_str(),
                                      body.length());
    status_.lastHttpCode = code;
    if (code > 0) {
        response = http.getString();
    }
    http.end();

    if (code <= 0) {
        // Negative codes are the client's own transport errors.
        status_.reachable = false;
        detail = String(F("cannot reach Spoolman: ")) +
                 HTTPClient::errorToString(code);
        noteError(detail.c_str());
        return false;
    }
    status_.reachable = true;

    if (code < 200 || code >= 300) {
        const String reported = spoolman_json::message(response);
        detail = String(F("Spoolman answered ")) + code;
        if (reported.length() > 0) {
            detail += F(": ");
            detail += reported;
        }
        noteError(detail.c_str());
        return false;
    }

    status_.lastError[0] = '\0';
    return true;
}

const char *SpoolmanClient::apiName(Api api) {
    switch (api) {
        case Api::Tags:
            return "tag table";
        case Api::ExtraField:
            return "nfc_uid field";
        default:
            return "not determined yet";
    }
}

void spoolmanNormalizeUid(const char *uid, char *out, size_t outSize) {
    size_t written = 0;
    for (const char *p = uid; p != nullptr && *p != '\0'; ++p) {
        const char c = *p;
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) {
            continue;  // separators, spaces, an 0x prefix's x
        }
        if (written + 1 < outSize) {
            out[written++] = (c >= 'a' && c <= 'f')
                                 ? static_cast<char>(c - 'a' + 'A')
                                 : c;
        }
    }
    if (outSize > 0) {
        out[written < outSize ? written : outSize - 1] = '\0';
    }
}

bool SpoolmanClient::scan(const char *uid, String &detail) {
    status_.lookedUp = false;
    status_.linkedSpoolId = 0;
    if (uid == nullptr || uid[0] == '\0') {
        detail = F("no tag UID to look up");
        return false;
    }

    char normalized[40] = {0};
    spoolmanNormalizeUid(uid, normalized, sizeof(normalized));
    if (normalized[0] == '\0') {
        detail = F("that tag UID is not hexadecimal");
        return false;
    }

    if (status_.api != Api::ExtraField) {
        bool unsupported = false;
        const bool ok = scanViaTagApi(normalized, unsupported, detail);
        if (!unsupported) {
            return ok;
        }
        // A Spoolman without the tag endpoints answers 404 to everything under
        // /api/v1/tag, so this is the version test and it only runs once.
        status_.api = Api::ExtraField;
    }
    return scanViaExtraField(normalized, detail);
}

bool SpoolmanClient::scanViaTagApi(const char *normalizedUid, bool &unsupported,
                                   String &detail) {
    unsupported = false;

    String body = F("{\"uid\":\"");
    body += normalizedUid;
    body += F("\",\"format\":\"openprinttag\"");
    if (readerName_[0] != '\0') {
        body += F(",\"name\":\"");
        body += readerName_;
        body += F("\"");
    }
    body += F("}");

    String response;
    if (!request("POST", F("/api/v1/tag/scan"), body, response, detail)) {
        // 404 and 405 are what an older Spoolman says about a path it does not
        // have; anything else is a real failure worth reporting.
        unsupported =
            status_.lastHttpCode == 404 || status_.lastHttpCode == 405;
        return false;
    }

    long spoolId = 0;
    bool wasNull = false;
    if (!spoolman_json::number(response, "matched_spool_id", spoolId, wasNull)) {
        detail = F("Spoolman's answer had no matched_spool_id in it");
        noteError(detail.c_str());
        return false;
    }

    status_.api = Api::Tags;
    status_.lookedUp = true;
    if (wasNull || spoolId <= 0) {
        long filamentId = 0;
        bool filamentNull = false;
        if (spoolman_json::number(response, "matched_filament_id", filamentId,
                                  filamentNull) &&
            !filamentNull && filamentId > 0) {
            detail = String(F("this tag is linked to filament ")) + filamentId +
                     F(" rather than to a spool, so there is nothing to weigh");
            return true;
        }
        detail = F("Spoolman does not know this tag yet");
        return true;
    }

    status_.linkedSpoolId = static_cast<int>(spoolId);
    detail = String(F("spool ")) + status_.linkedSpoolId;
    return true;
}

bool SpoolmanClient::scanViaExtraField(const char *normalizedUid,
                                       String &detail) {
    String path = F("/api/v1/spool?allow_archived=false&extra.nfc_uid=");
    path += normalizedUid;

    String response;
    if (!request("GET", path, String(), response, detail)) {
        return false;
    }

    const int elements = spoolman_json::arrayLength(response);
    if (elements == 0) {
        status_.lookedUp = true;
        detail = F("Spoolman does not know this tag yet");
        return true;
    }

    // Spoolman drops a filter on an extra field it has never been told about,
    // and answering a filtered query with the entire inventory is exactly how a
    // weight would end up on somebody else's spool. Both checks below exist for
    // that one failure.
    if (response.indexOf(normalizedUid) < 0) {
        detail = F("Spoolman answered with spools that do not carry this tag's "
                   "UID, so the filter was ignored: link a tag once from this "
                   "page to have the nfc_uid field created");
        noteError(detail.c_str());
        return false;
    }
    if (elements > 1) {
        detail = String(F("this UID is on ")) + elements +
                 F(" spools in Spoolman, which has to be sorted out there "
                   "before a weight can be sent");
        noteError(detail.c_str());
        return false;
    }

    long spoolId = 0;
    bool wasNull = false;
    if (!spoolman_json::numberAtDepth(response, "id", 2, spoolId, wasNull) ||
        wasNull || spoolId <= 0) {
        detail = F("Spoolman's answer had no spool id in it");
        noteError(detail.c_str());
        return false;
    }

    status_.api = Api::ExtraField;
    status_.lookedUp = true;
    status_.linkedSpoolId = static_cast<int>(spoolId);
    detail = String(F("spool ")) + status_.linkedSpoolId;
    return true;
}

bool SpoolmanClient::measure(float grossGrams, String &detail) {
    if (status_.linkedSpoolId <= 0) {
        detail = F("no Spoolman spool is linked to this tag");
        return false;
    }
    if (!(grossGrams > 0.0f)) {
        detail = F("refusing to send a weight of zero or less");
        return false;
    }

    String path = F("/api/v1/spool/");
    path += status_.linkedSpoolId;
    path += F("/measure");

    String body = F("{\"weight\":");
    body += String(grossGrams, 1);
    body += F("}");

    String response;
    if (!request("PUT", path, body, response, detail)) {
        return false;
    }

    ++status_.measureCount;
    status_.lastMeasureGrams = grossGrams;
    status_.lastMeasureMs = millis();

    // Spoolman answers with the updated spool, which says what it made of the
    // measurement. Worth reporting back: it is the only confirmation that the
    // tare Spoolman holds agrees with the one on the tag.
    long used = 0;
    bool wasNull = false;
    detail = String(F("sent ")) + String(grossGrams, 0) + F(" g");
    if (spoolman_json::number(response, "used_weight", used, wasNull) &&
        !wasNull) {
        detail += String(F(", Spoolman now has ")) + used + F(" g used");
    }
    return true;
}

bool SpoolmanClient::link(int spoolId, const char *uid, String &detail) {
    if (spoolId <= 0) {
        detail = F("a spool number is needed");
        return false;
    }
    if (uid == nullptr || uid[0] == '\0') {
        detail = F("no tag in the field to link");
        return false;
    }

    char normalized[40] = {0};
    spoolmanNormalizeUid(uid, normalized, sizeof(normalized));
    if (normalized[0] == '\0') {
        detail = F("that tag UID is not hexadecimal");
        return false;
    }

    bool ok = false;
    if (status_.api != Api::ExtraField) {
        bool unsupported = false;
        ok = linkViaTagApi(spoolId, normalized, unsupported, detail);
        if (unsupported) {
            status_.api = Api::ExtraField;
            ok = linkViaExtraField(spoolId, normalized, detail);
        }
    } else {
        ok = linkViaExtraField(spoolId, normalized, detail);
    }
    if (!ok) {
        return false;
    }

    status_.linkedSpoolId = spoolId;
    status_.lookedUp = true;
    detail = String(F("this tag is now spool ")) + spoolId + F(" in Spoolman");
    return true;
}

bool SpoolmanClient::linkViaTagApi(int spoolId, const char *normalizedUid,
                                   bool &unsupported, String &detail) {
    unsupported = false;

    String path = F("/api/v1/spool/");
    path += spoolId;
    path += F("/tag");

    String body = F("{\"uid\":\"");
    body += normalizedUid;
    body += F("\",\"format\":\"openprinttag\"}");

    String response;
    if (!request("PUT", path, body, response, detail)) {
        unsupported =
            status_.lastHttpCode == 404 || status_.lastHttpCode == 405;
        return false;
    }
    status_.api = Api::Tags;
    return true;
}

bool SpoolmanClient::linkViaExtraField(int spoolId, const char *normalizedUid,
                                       String &detail) {
    if (!ensureExtraField(detail)) {
        return false;
    }

    String path = F("/api/v1/spool/");
    path += spoolId;

    // Every value in Spoolman's `extra` map is itself JSON, so a text field's
    // value is a quoted string inside the string. Getting this wrong stores the
    // UID with no quotes, which the filter then never matches.
    String body = F("{\"extra\":{\"nfc_uid\":\"\\\"");
    body += normalizedUid;
    body += F("\\\"\"}}");

    String response;
    return request("PATCH", path, body, response, detail);
}

bool SpoolmanClient::ensureExtraField(String &detail) {
    if (extraFieldRegistered_) {
        return true;
    }
    String response;
    const bool ok = request(
        "POST", F("/api/v1/field/spool/nfc_uid"),
        F("{\"name\":\"NFC tag UID\",\"field_type\":\"text\",\"order\":0}"),
        response, detail);
    if (ok) {
        extraFieldRegistered_ = true;
    }
    return ok;
}

bool SpoolmanClient::unlink(int spoolId, const char *uid, String &detail) {
    if (spoolId <= 0 || uid == nullptr || uid[0] == '\0') {
        detail = F("a spool number and a tag are both needed");
        return false;
    }

    char normalized[40] = {0};
    spoolmanNormalizeUid(uid, normalized, sizeof(normalized));

    String response;
    bool ok = false;
    if (status_.api == Api::ExtraField) {
        String path = F("/api/v1/spool/");
        path += spoolId;
        ok = request("PATCH", path, F("{\"extra\":{\"nfc_uid\":null}}"),
                     response, detail);
    } else {
        String path = F("/api/v1/spool/");
        path += spoolId;
        path += F("/tag/");
        path += normalized;
        ok = request("DELETE", path, String(), response, detail);
    }
    if (!ok) {
        return false;
    }

    if (status_.linkedSpoolId == spoolId) {
        forgetTag();
    }
    detail = String(F("unlinked from spool ")) + spoolId;
    return true;
}
