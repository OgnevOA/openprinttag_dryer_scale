// Tag-level NFC logic: polling, the cached tag image, and the write paths.
//
// Two kinds of write exist and they are deliberately very different:
//
//  * provision() initialises a blank tag, writing the whole image once.
//  * updateAux() maintains consumed_weight during use, writing only the 4-byte
//    blocks whose contents actually changed and never touching a byte outside
//    the auxiliary region.
//
// Every write is read back and compared before it is reported as successful.

#pragma once

#include <Arduino.h>

#include "../config.h"
#include "Iso15693Reader.h"
#include "openprinttag/opt_tag.h"

class NfcManager {
public:
    struct Status {
        bool readerOnline = false;
        bool pollEnabled = true;
        bool tagPresent = false;
        bool tagDecoded = false;
        bool writeProtected = false;
        bool multiBlockReads = true;

        char uid[17] = {0};          // hex, most significant byte first
        uint8_t blockSize = 0;
        uint8_t blockCount = 0;
        uint16_t imageBytes = 0;
        opt::OptStatus decodeStatus = opt::OptStatus::Ok;

        uint32_t lastReadMs = 0;
        uint32_t lastWriteMs = 0;
        uint32_t readCount = 0;
        uint32_t readErrorCount = 0;
        uint32_t writeCount = 0;
        uint32_t blocksWritten = 0;
        uint32_t writeErrorCount = 0;

        // Poll outcomes, which is how the bracket's position gets tuned: with a
        // spool sitting in the dryer, anything short of every poll finding the
        // tag means the coil is not close enough or not concentric enough.
        uint32_t pollCount = 0;
        uint32_t pollHitCount = 0;
        uint32_t recentPollMask = 0;  // bit 0 is the most recent poll
        uint8_t recentPollCount = 0;  // how many of those bits mean anything

        uint8_t recentPollHits() const;

        char readerVersion[32] = {0};
        char lastError[72] = {0};
    };

    bool begin();

    // Polls on a timer. Returns true when the tag's identity or contents
    // changed, which is the caller's cue to recompute and republish.
    bool loop(uint32_t nowMs);

    // Forces a full read now, regardless of the poll timer.
    bool refresh();

    const Status &status() const { return status_; }
    const opt::OptTag &tag() const { return tag_; }
    const uint8_t *image() const { return image_; }
    size_t imageLength() const { return status_.imageBytes; }

    // True while the RF field is energised. The scale discards samples taken
    // during that window: 100 mA of field driver next to a 20-bit ADC is not
    // something to average over.
    bool rfActive() const { return rfActive_; }

    void setPollEnabled(bool enabled) {
        pollEnabled_ = enabled;
        status_.pollEnabled = enabled;
    }
    bool pollEnabled() const { return pollEnabled_; }

    // Updates the auxiliary region. `message` receives a human-readable result
    // either way, including how many blocks were written.
    bool updateAux(const opt::OptAuxUpdate &update, char *message,
                   size_t messageLen);

    // Initialises a blank tag from `request`. The tag's real capacity replaces
    // whatever the request asked for, then the result is read back and decoded
    // before being reported as successful.
    bool provision(const opt::OptBuildRequest &request, char *message,
                   size_t messageLen);

    // Reader and tag diagnostics for the self-test page.
    void selfTest(String &report);

    // Runs `attempts` inventories back to back and reports how many found the
    // tag and how long each took, then times one full image read. This is the
    // measurement that decides where on the lid the bracket goes.
    void readTest(uint16_t attempts, String &report);

    // How many of the last polls are remembered for the recent hit rate.
    static constexpr uint8_t kRecentPollWindow = 20;

private:
    bool rfOn();
    void rfOff();
    bool inventory(uint8_t *uid);
    void notePoll(bool foundTag);
    bool readWholeTag(uint8_t *uid);
    bool writeByteRange(const uint8_t *newImage, size_t firstByte,
                        size_t lastByte, char *message, size_t messageLen);
    void setError(const char *format, ...);
    void formatUid(const uint8_t *uid);
    void clearTag();

    Iso15693Reader reader_{PIN_PN5180_NSS, PIN_PN5180_BUSY, PIN_PN5180_RST};
    Status status_;
    opt::OptTag tag_;
    uint8_t image_[opt::kMaxTagImage] = {0};
    uint8_t scratch_[opt::kMaxTagImage] = {0};
    uint8_t uid_[8] = {0};

    bool pollEnabled_ = true;
    bool rfActive_ = false;
    uint32_t nextPollMs_ = 0;
    uint32_t nextReaderRetryMs_ = 0;
    uint8_t absentPolls_ = 0;
};
