#include "NfcManager.h"

#include <stdarg.h>

namespace {

// ICODE SLIX2 geometry, used when a tag answers inventory but refuses Get
// System Information.
constexpr uint8_t kAssumedBlockSize = 4;
constexpr uint8_t kAssumedBlockCount = 79;

// How long to wait before trying to bring an unresponsive reader back.
constexpr uint32_t kReaderRetryIntervalMs = 10000;

const char *errorText(ISO15693ErrorCode code) {
    switch (code) {
    case EC_NO_CARD: return "no answer from tag";
    case ISO15693_EC_OK: return "ok";
    case ISO15693_EC_NOT_SUPPORTED: return "command not supported";
    case ISO15693_EC_NOT_RECOGNIZED: return "command not recognised";
    case ISO15693_EC_OPTION_NOT_SUPPORTED: return "option not supported";
    case ISO15693_EC_UNKNOWN_ERROR: return "unknown tag error";
    case ISO15693_EC_BLOCK_NOT_AVAILABLE: return "block not available";
    case ISO15693_EC_BLOCK_ALREADY_LOCKED: return "block already locked";
    case ISO15693_EC_BLOCK_IS_LOCKED: return "block is write protected";
    case ISO15693_EC_BLOCK_NOT_PROGRAMMED: return "block not programmed";
    case ISO15693_EC_BLOCK_NOT_LOCKED: return "block not locked";
    case ISO15693_EC_CUSTOM_CMD_ERROR: return "custom command error";
    }
    return "unexpected tag error";
}

}  // namespace

bool NfcManager::begin() {
    reader_.begin(PIN_PN5180_SCK, PIN_PN5180_MISO, PIN_PN5180_MOSI,
                  PN5180_SPI_HZ);

    if (!reader_.reset()) {
        setError("reader did not come out of reset");
        status_.readerOnline = false;
        return false;
    }

    uint8_t productMajor = 0;
    uint8_t productMinor = 0;
    uint8_t firmwareMajor = 0;
    uint8_t firmwareMinor = 0;
    if (!reader_.readVersions(productMajor, productMinor, firmwareMajor,
                              firmwareMinor)) {
        setError("reader version read failed; check 5 V and the ribbon cable");
        status_.readerOnline = false;
        return false;
    }

    snprintf(status_.readerVersion, sizeof(status_.readerVersion),
             "PN5180 %u.%u fw %u.%u", productMajor, productMinor,
             firmwareMajor, firmwareMinor);
    status_.readerOnline = true;
    status_.lastError[0] = '\0';
    return true;
}

bool NfcManager::rfOn() {
    if (rfActive_) {
        return true;
    }
    if (!reader_.setupRF()) {
        setError("could not enable the RF field");
        status_.readerOnline = false;
        return false;
    }
    rfActive_ = true;
    return true;
}

void NfcManager::rfOff() {
    if (!rfActive_) {
        return;
    }
    reader_.setRF_off();
    rfActive_ = false;
}

void NfcManager::formatUid(const uint8_t *uid) {
    // ISO 15693 sends the UID least significant byte first; everyone displays
    // it the other way round, starting with the E0 manufacturer prefix.
    for (int i = 0; i < 8; ++i) {
        snprintf(status_.uid + i * 2, 3, "%02X", uid[7 - i]);
    }
    status_.uid[16] = '\0';
}

void NfcManager::setError(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(status_.lastError, sizeof(status_.lastError), format, args);
    va_end(args);
}

void NfcManager::clearTag() {
    status_.tagPresent = false;
    status_.tagDecoded = false;
    status_.writeProtected = false;
    status_.imageBytes = 0;
    status_.uid[0] = '\0';
    status_.blockSize = 0;
    status_.blockCount = 0;
    tag_ = opt::OptTag{};
    memset(uid_, 0, sizeof(uid_));
}

bool NfcManager::inventory(uint8_t *uid) {
    const ISO15693ErrorCode rc = reader_.getInventory(uid);
    if (rc == ISO15693_EC_OK) {
        return true;
    }
    if (rc != EC_NO_CARD) {
        setError("inventory failed: %s", errorText(rc));
    }
    return false;
}

void NfcManager::notePoll(bool foundTag) {
    ++status_.pollCount;
    if (foundTag) {
        ++status_.pollHitCount;
    }

    // The last few outcomes matter more than the lifetime total when aiming the
    // reader: the total includes every hour the dryer stood empty.
    status_.recentPollMask =
        (status_.recentPollMask << 1) | (foundTag ? 1u : 0u);
    if (status_.recentPollCount < kRecentPollWindow) {
        ++status_.recentPollCount;
    }
}

bool NfcManager::readWholeTag(uint8_t *uid) {
    uint8_t blockSize = 0;
    uint8_t blockCount = 0;
    const ISO15693ErrorCode info =
        reader_.getSystemInfo(uid, &blockSize, &blockCount);
    if (info != ISO15693_EC_OK || blockSize == 0 || blockCount == 0) {
        // Not fatal: some tags answer inventory but not Get System Information.
        // An OpenPrintTag is a SLIX2, so assume that geometry and carry on.
        blockSize = kAssumedBlockSize;
        blockCount = kAssumedBlockCount;
    }

    size_t bytes = static_cast<size_t>(blockSize) * blockCount;
    if (bytes > sizeof(image_)) {
        bytes = (sizeof(image_) / blockSize) * blockSize;
        blockCount = static_cast<uint8_t>(bytes / blockSize);
    }

    const ISO15693ErrorCode rc =
        reader_.readBlocks(uid, 0, blockCount, image_, blockSize);
    if (rc != ISO15693_EC_OK) {
        ++status_.readErrorCount;
        setError("read failed: %s", errorText(rc));
        return false;
    }

    status_.blockSize = blockSize;
    status_.blockCount = blockCount;
    status_.imageBytes = static_cast<uint16_t>(bytes);
    status_.multiBlockReads = !reader_.multiBlockUnsupported();
    ++status_.readCount;
    status_.lastReadMs = millis();

    status_.decodeStatus = opt::optParse(image_, bytes, tag_);
    status_.tagDecoded = status_.decodeStatus == opt::OptStatus::Ok;
    if (!status_.tagDecoded) {
        setError("tag data not readable: %s",
                 opt::optStatusText(status_.decodeStatus));
    } else {
        status_.writeProtected =
            tag_.main.hasWriteProtection && tag_.main.writeProtection != 0;
        status_.lastError[0] = '\0';
    }
    return status_.tagDecoded;
}

bool NfcManager::loop(uint32_t nowMs) {
    if (!pollEnabled_) {
        return false;
    }

    if (!status_.readerOnline) {
        if (static_cast<int32_t>(nowMs - nextReaderRetryMs_) < 0) {
            return false;
        }
        nextReaderRetryMs_ = nowMs + kReaderRetryIntervalMs;
        rfActive_ = false;
        if (!begin()) {
            return false;
        }
    }

    if (static_cast<int32_t>(nowMs - nextPollMs_) < 0) {
        return false;
    }
    nextPollMs_ = nowMs + NFC_POLL_INTERVAL_MS;

    if (!rfOn()) {
        return false;
    }

    bool changed = false;
    uint8_t uid[8] = {0};
    const bool found = inventory(uid);
    notePoll(found);
    if (!found) {
        if (status_.tagPresent) {
            if (++absentPolls_ >= NFC_ABSENT_CONFIRM_COUNT) {
                clearTag();
                changed = true;
            }
        }
        rfOff();
        return changed;
    }

    absentPolls_ = 0;
    const bool sameTag = status_.tagPresent &&
                         memcmp(uid, uid_, sizeof(uid_)) == 0;
    if (!sameTag || !status_.tagDecoded) {
        memcpy(uid_, uid, sizeof(uid_));
        formatUid(uid);
        status_.tagPresent = true;
        readWholeTag(uid_);
        changed = true;
    }

    rfOff();
    return changed;
}

bool NfcManager::refresh() {
    if (!status_.readerOnline && !begin()) {
        return false;
    }
    if (!rfOn()) {
        return false;
    }

    uint8_t uid[8] = {0};
    if (!inventory(uid)) {
        if (status_.tagPresent) {
            clearTag();
        }
        rfOff();
        return false;
    }

    memcpy(uid_, uid, sizeof(uid_));
    formatUid(uid);
    status_.tagPresent = true;
    const bool ok = readWholeTag(uid_);
    rfOff();
    return ok;
}

bool NfcManager::writeByteRange(const uint8_t *newImage, size_t firstByte,
                                size_t lastByte, char *message,
                                size_t messageLen) {
    const uint8_t blockSize = status_.blockSize ? status_.blockSize
                                                : kAssumedBlockSize;
    const size_t firstBlock = firstByte / blockSize;
    const size_t lastBlock = lastByte / blockSize;

    for (size_t block = firstBlock; block <= lastBlock; ++block) {
        const uint8_t *source = newImage + block * blockSize;

        // Skip blocks that already hold the wanted bytes: a spool sitting in
        // the dryer for months should spend as few of its 100k write cycles as
        // possible.
        if (memcmp(source, image_ + block * blockSize, blockSize) == 0) {
            continue;
        }

        uint8_t buffer[32];
        memcpy(buffer, source, blockSize);
        const ISO15693ErrorCode rc = reader_.writeSingleBlock(
            uid_, static_cast<uint8_t>(block), buffer, blockSize);
        if (rc != ISO15693_EC_OK) {
            ++status_.writeErrorCount;
            if (rc == ISO15693_EC_BLOCK_IS_LOCKED ||
                rc == ISO15693_EC_BLOCK_ALREADY_LOCKED) {
                status_.writeProtected = true;
            }
            setError("block %u write failed: %s", static_cast<unsigned>(block),
                     errorText(rc));
            snprintf(message, messageLen, "write failed at block %u: %s",
                     static_cast<unsigned>(block), errorText(rc));
            return false;
        }

        // Read the block back before believing it.
        uint8_t verify[32] = {0};
        const ISO15693ErrorCode check = reader_.readSingleBlock(
            uid_, static_cast<uint8_t>(block), verify, blockSize);
        if (check != ISO15693_EC_OK ||
            memcmp(verify, source, blockSize) != 0) {
            ++status_.writeErrorCount;
            setError("block %u verify failed", static_cast<unsigned>(block));
            snprintf(message, messageLen, "verify failed at block %u",
                     static_cast<unsigned>(block));
            return false;
        }

        memcpy(image_ + block * blockSize, source, blockSize);
        ++status_.blocksWritten;
    }

    ++status_.writeCount;
    status_.lastWriteMs = millis();
    return true;
}

bool NfcManager::updateAux(const opt::OptAuxUpdate &update, char *message,
                           size_t messageLen) {
    message[0] = '\0';

    if (!status_.tagPresent || !status_.tagDecoded) {
        snprintf(message, messageLen, "no readable tag present");
        return false;
    }
    if (!tag_.regions.hasAux) {
        snprintf(message, messageLen,
                 "tag has no auxiliary region, so it cannot be updated");
        return false;
    }
    if (status_.writeProtected) {
        snprintf(message, messageLen, "tag reports write protection");
        return false;
    }

    const size_t auxStart = tag_.regions.auxAbsolute();
    const size_t auxSize = tag_.regions.auxSize;
    if (auxStart + auxSize > status_.imageBytes) {
        snprintf(message, messageLen, "auxiliary region outside the tag");
        return false;
    }

    uint8_t encoded[128];
    size_t encodedLen = 0;
    const opt::OptStatus built = opt::optBuildAuxRegion(
        image_ + auxStart, auxSize, update, encoded, sizeof(encoded),
        encodedLen);
    if (built != opt::OptStatus::Ok || encodedLen > auxSize) {
        snprintf(message, messageLen, "auxiliary data does not fit (%u > %u B)",
                 static_cast<unsigned>(encodedLen),
                 static_cast<unsigned>(auxSize));
        return false;
    }

    // Candidate image: everything as it is, with the re-encoded region dropped
    // in and the rest of the region zeroed, so a field that shrank does not
    // leave stale bytes behind.
    memcpy(scratch_, image_, status_.imageBytes);
    memset(scratch_ + auxStart, 0, auxSize);
    memcpy(scratch_ + auxStart, encoded, encodedLen);

    size_t firstDiff = 0;
    size_t blockCount = 0;
    if (!opt::optDiffBlocks(image_ + auxStart, scratch_ + auxStart, auxSize, 1,
                            firstDiff, blockCount)) {
        snprintf(message, messageLen, "already up to date, nothing written");
        return true;
    }
    const size_t lastDiff = firstDiff + blockCount - 1;

    if (!status_.readerOnline && !begin()) {
        snprintf(message, messageLen, "reader offline");
        return false;
    }
    if (!rfOn()) {
        snprintf(message, messageLen, "could not enable the RF field");
        return false;
    }

    // Confirm the same tag is still in the field before writing to it.
    uint8_t uid[8] = {0};
    if (!inventory(uid) || memcmp(uid, uid_, sizeof(uid_)) != 0) {
        rfOff();
        snprintf(message, messageLen, "tag left the field before the write");
        return false;
    }

    const uint32_t before = status_.blocksWritten;
    const bool ok = writeByteRange(scratch_, auxStart + firstDiff,
                                  auxStart + lastDiff, message, messageLen);
    if (ok) {
        status_.decodeStatus = opt::optParse(image_, status_.imageBytes, tag_);
        status_.tagDecoded = status_.decodeStatus == opt::OptStatus::Ok;
        snprintf(message, messageLen, "wrote %u block%s",
                 static_cast<unsigned>(status_.blocksWritten - before),
                 (status_.blocksWritten - before) == 1 ? "" : "s");
    }
    rfOff();
    return ok;
}

bool NfcManager::provision(const opt::OptBuildRequest &request, char *message,
                           size_t messageLen) {
    message[0] = '\0';

    if (!status_.readerOnline && !begin()) {
        snprintf(message, messageLen, "reader offline");
        return false;
    }
    if (!rfOn()) {
        snprintf(message, messageLen, "could not enable the RF field");
        return false;
    }

    uint8_t uid[8] = {0};
    if (!inventory(uid)) {
        rfOff();
        snprintf(message, messageLen, "no tag in the field");
        return false;
    }
    memcpy(uid_, uid, sizeof(uid_));
    formatUid(uid);
    status_.tagPresent = true;

    uint8_t blockSize = 0;
    uint8_t blockCount = 0;
    if (reader_.getSystemInfo(uid_, &blockSize, &blockCount) !=
            ISO15693_EC_OK ||
        blockSize == 0 || blockCount == 0) {
        blockSize = kAssumedBlockSize;
        blockCount = kAssumedBlockCount;
    }
    status_.blockSize = blockSize;
    status_.blockCount = blockCount;

    size_t capacity = static_cast<size_t>(blockSize) * blockCount;
    if (capacity > sizeof(image_)) {
        capacity = (sizeof(image_) / blockSize) * blockSize;
        blockCount = static_cast<uint8_t>(capacity / blockSize);
        status_.blockCount = blockCount;
    }

    // Read the tag first: the current contents decide which blocks need
    // writing, and a factory-programmed tag should not be overwritten silently.
    const ISO15693ErrorCode rc =
        reader_.readBlocks(uid_, 0, blockCount, image_, blockSize);
    if (rc != ISO15693_EC_OK) {
        rfOff();
        snprintf(message, messageLen, "could not read the tag first: %s",
                 errorText(rc));
        return false;
    }
    status_.imageBytes = static_cast<uint16_t>(capacity);

    opt::OptBuildRequest sized = request;
    sized.tagCapacity = capacity;  // the tag's real size wins

    size_t imageLen = 0;
    opt::OptRegions regions;
    const opt::OptStatus built = opt::optBuildImage(
        sized, scratch_, sizeof(scratch_), imageLen, &regions);
    if (built != opt::OptStatus::Ok) {
        rfOff();
        snprintf(message, messageLen, "could not build the tag image: %s",
                 opt::optStatusText(built));
        return false;
    }

    // Anything past the built image is cleared, so a re-provisioned tag carries
    // no leftovers from its previous contents.
    if (capacity > imageLen) {
        memset(scratch_ + imageLen, 0, capacity - imageLen);
    }

    const uint32_t before = status_.blocksWritten;
    if (!writeByteRange(scratch_, 0, capacity - 1, message, messageLen)) {
        rfOff();
        return false;
    }

    // Read the whole tag back from scratch and decode it: the only proof that
    // matters is that the tag now reads as the spool it should be.
    const bool reread = readWholeTag(uid_);
    rfOff();

    if (!reread) {
        snprintf(message, messageLen, "written, but the read-back failed: %s",
                 opt::optStatusText(status_.decodeStatus));
        return false;
    }

    const bool weightMatches =
        tag_.main.hasActualNettoFullWeight ==
            request.main.hasActualNettoFullWeight &&
        (!request.main.hasActualNettoFullWeight ||
         fabsf(tag_.main.actualNettoFullWeight -
               request.main.actualNettoFullWeight) < 0.5f);
    const bool nameMatches =
        strcmp(tag_.main.materialName, request.main.materialName) == 0;
    if (!weightMatches || !nameMatches) {
        snprintf(message, messageLen,
                 "read-back disagrees with what was written");
        return false;
    }

    snprintf(message, messageLen,
             "provisioned %s, %u block%s written, read back and verified",
             tag_.main.materialName,
             static_cast<unsigned>(status_.blocksWritten - before),
             (status_.blocksWritten - before) == 1 ? "" : "s");
    return true;
}

uint8_t NfcManager::Status::recentPollHits() const {
    const uint32_t meaningful =
        recentPollCount >= 32 ? recentPollMask
                              : recentPollMask & ((1UL << recentPollCount) - 1);
    uint8_t hits = 0;
    for (uint32_t bits = meaningful; bits != 0; bits >>= 1) {
        hits += bits & 1u;
    }
    return hits;
}

void NfcManager::selfTest(String &report) {
    report += F("PN5180 reader\n");
    if (!begin()) {
        report += F("  FAIL: ");
        report += status_.lastError;
        report += '\n';
        return;
    }
    report += F("  version: ");
    report += status_.readerVersion;
    report += '\n';

    if (status_.recentPollCount > 0) {
        report += F("  recent polls: ");
        report += status_.recentPollHits();
        report += F(" of ");
        report += status_.recentPollCount;
        report += F(" found a tag\n");
    }

    if (!rfOn()) {
        report += F("  FAIL: RF field would not start\n");
        return;
    }
    report += F("  RF field: on\n");

    uint8_t uid[8] = {0};
    if (!inventory(uid)) {
        report += F("  no tag in the field\n");
        rfOff();
        return;
    }
    formatUid(uid);
    memcpy(uid_, uid, sizeof(uid_));
    report += F("  tag UID: ");
    report += status_.uid;
    report += '\n';

    uint8_t blockSize = 0;
    uint8_t blockCount = 0;
    const ISO15693ErrorCode info =
        reader_.getSystemInfo(uid_, &blockSize, &blockCount);
    if (info == ISO15693_EC_OK) {
        report += F("  geometry: ");
        report += blockCount;
        report += F(" blocks of ");
        report += blockSize;
        report += F(" B = ");
        report += static_cast<int>(blockSize) * blockCount;
        report += F(" B\n");
    } else {
        report += F("  get system info failed: ");
        report += errorText(info);
        report += '\n';
    }

    const bool read = readWholeTag(uid_);
    report += F("  full read: ");
    report += read ? F("decoded as an OpenPrintTag\n") : F("failed\n");
    if (!read) {
        report += F("    ");
        report += status_.lastError;
        report += '\n';
    } else {
        report += F("    ");
        report += tag_.main.brandName;
        report += ' ';
        report += tag_.main.materialName;
        report += '\n';
        report += F("    multi-block reads: ");
        report += status_.multiBlockReads ? F("yes\n") : F("no, using single\n");
        report += F("    write protection: ");
        report += status_.writeProtected ? F("yes\n") : F("none\n");
    }
    rfOff();
}

void NfcManager::readTest(uint16_t attempts, String &report) {
    if (attempts < 1) {
        attempts = 1;
    } else if (attempts > 200) {
        attempts = 200;
    }

    report += F("Read test, ");
    report += attempts;
    report += F(" attempts\n");

    if (!status_.readerOnline && !begin()) {
        report += F("  FAIL: ");
        report += status_.lastError;
        report += '\n';
        return;
    }
    if (!rfOn()) {
        report += F("  FAIL: RF field would not start\n");
        return;
    }

    uint16_t hits = 0;
    uint32_t totalMs = 0;
    uint32_t worstMs = 0;
    uint32_t bestMs = 0xFFFFFFFFUL;
    uint8_t uid[8] = {0};
    uint8_t lastUid[8] = {0};
    bool mixedUids = false;

    for (uint16_t i = 0; i < attempts; ++i) {
        const uint32_t startedMs = millis();
        const bool found = inventory(uid);
        const uint32_t tookMs = millis() - startedMs;
        if (found) {
            if (hits > 0 && memcmp(uid, lastUid, sizeof(uid)) != 0) {
                mixedUids = true;
            }
            memcpy(lastUid, uid, sizeof(lastUid));
            ++hits;
            totalMs += tookMs;
            worstMs = max(worstMs, tookMs);
            bestMs = min(bestMs, tookMs);
        }
        // The reader is not the only thing on this board; a tight loop on it
        // would starve the rest of the sketch of its slice of time.
        delay(20);
    }

    report += F("  found the tag ");
    report += hits;
    report += F(" of ");
    report += attempts;
    report += F(" times (");
    report += static_cast<int>((100UL * hits + attempts / 2) / attempts);
    report += F("%)\n");

    if (hits == 0) {
        report += F("  nothing answered: no tag in the field, or the coil is "
                    "too far from it\n");
        rfOff();
        return;
    }

    report += F("  inventory time: ");
    report += bestMs;
    report += F(" ms best, ");
    report += totalMs / hits;
    report += F(" ms average, ");
    report += worstMs;
    report += F(" ms worst\n");

    if (mixedUids) {
        report += F("  WARNING: more than one tag answered, so these numbers "
                    "are about two tags competing\n");
    }

    memcpy(uid_, lastUid, sizeof(uid_));
    formatUid(uid_);
    status_.tagPresent = true;

    const uint32_t readStartedMs = millis();
    const bool read = readWholeTag(uid_);
    const uint32_t readMs = millis() - readStartedMs;
    report += F("  full image read: ");
    if (read) {
        report += status_.imageBytes;
        report += F(" B in ");
        report += readMs;
        report += F(" ms, decoded\n");
    } else {
        report += F("failed after ");
        report += readMs;
        report += F(" ms: ");
        report += status_.lastError;
        report += '\n';
    }

    if (hits < attempts) {
        report += F("  a rate below 100% means the bracket wants moving: "
                    "centre the coil on the spool axis first, then close the "
                    "gap to the tag\n");
    }
    rfOff();
}
