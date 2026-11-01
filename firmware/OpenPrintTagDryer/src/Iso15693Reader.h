// Hardware-level ISO 15693 reader: the vendored PN5180 driver plus the pieces
// this project needs and upstream does not have.
//
// Read Multiple Blocks is the important one. A 316-byte OpenPrintTag is 79
// blocks, and one command per block costs over a second of RF time with the
// vendored driver's per-transfer delays. In chunks it is roughly a tenth of
// that, which is what makes polling through the dryer lid practical.

#pragma once

#include <Arduino.h>

#include "pn5180/PN5180ISO15693.h"

class Iso15693Reader : public PN5180ISO15693 {
public:
    Iso15693Reader(uint8_t nssPin, uint8_t busyPin, uint8_t rstPin)
        : PN5180ISO15693(nssPin, busyPin, rstPin) {}

    // Blocks per Read Multiple Blocks command. Conservative: NXP's ICODE parts
    // accept more, but a long response is also a longer window for the tag to
    // slip out of the field mid-answer.
    static const uint8_t kMaxChunkBlocks = 16;

    // Reads `count` blocks starting at `firstBlock` in as few commands as the
    // tag allows, falling back to single-block reads if the tag rejects the
    // multi-block command.
    ISO15693ErrorCode readBlocks(uint8_t *uid, uint8_t firstBlock,
                                 uint8_t count, uint8_t *data,
                                 uint8_t blockSize);

    // Single Read Multiple Blocks command. Exposed mainly so a caller can
    // discover whether the tag supports it at all.
    ISO15693ErrorCode readMultipleBlocks(uint8_t *uid, uint8_t firstBlock,
                                         uint8_t count, uint8_t *data,
                                         uint8_t blockSize);

    // Reader identification, useful as a "is anything actually connected"
    // probe: an absent or unpowered module reads back all ones or all zeros.
    bool readVersions(uint8_t &productMajor, uint8_t &productMinor,
                      uint8_t &firmwareMajor, uint8_t &firmwareMinor);

    // True once a multi-block read has failed, after which this reader stops
    // trying the command for the rest of the session.
    bool multiBlockUnsupported() const { return multiBlockFailed; }

private:
    bool multiBlockFailed = false;
};
