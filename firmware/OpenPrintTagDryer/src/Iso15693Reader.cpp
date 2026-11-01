#include "Iso15693Reader.h"

ISO15693ErrorCode Iso15693Reader::readMultipleBlocks(uint8_t *uid,
                                                    uint8_t firstBlock,
                                                    uint8_t count,
                                                    uint8_t *data,
                                                    uint8_t blockSize) {
    if (count == 0) {
        return ISO15693_EC_OK;
    }

    // Flags 0x22: addressed by UID, high data rate. Command 0x23: Read
    // Multiple Blocks. The block count travels as count-1.
    uint8_t command[12] = {0x22, 0x23, 0, 0, 0, 0, 0, 0, 0, 0, firstBlock,
                           static_cast<uint8_t>(count - 1)};
    for (int i = 0; i < 8; ++i) {
        command[2 + i] = uid[i];  // UID goes out least significant byte first
    }

    uint8_t *result = nullptr;
    const ISO15693ErrorCode rc =
        issueISO15693Command(command, sizeof(command), &result);
    if (rc != ISO15693_EC_OK) {
        return rc;
    }

    // Response: response flags, then the block data back to back.
    memcpy(data, result + 1, static_cast<size_t>(count) * blockSize);
    return ISO15693_EC_OK;
}

ISO15693ErrorCode Iso15693Reader::readBlocks(uint8_t *uid, uint8_t firstBlock,
                                            uint8_t count, uint8_t *data,
                                            uint8_t blockSize) {
    uint8_t done = 0;
    while (done < count) {
        const uint8_t remaining = static_cast<uint8_t>(count - done);
        const uint8_t chunk =
            remaining > kMaxChunkBlocks ? kMaxChunkBlocks : remaining;
        uint8_t *destination = data + static_cast<size_t>(done) * blockSize;

        if (!multiBlockFailed) {
            const ISO15693ErrorCode rc = readMultipleBlocks(
                uid, static_cast<uint8_t>(firstBlock + done), chunk,
                destination, blockSize);
            if (rc == ISO15693_EC_OK) {
                done = static_cast<uint8_t>(done + chunk);
                continue;
            }
            if (rc == EC_NO_CARD) {
                return rc;  // tag gone: retrying single blocks will not help
            }
            // The tag understood the frame but refused the command, so stop
            // using it and read the rest one block at a time.
            multiBlockFailed = true;
        }

        for (uint8_t i = 0; i < chunk; ++i) {
            const ISO15693ErrorCode rc = readSingleBlock(
                uid, static_cast<uint8_t>(firstBlock + done + i),
                destination + static_cast<size_t>(i) * blockSize, blockSize);
            if (rc != ISO15693_EC_OK) {
                return rc;
            }
        }
        done = static_cast<uint8_t>(done + chunk);
    }
    return ISO15693_EC_OK;
}

bool Iso15693Reader::readVersions(uint8_t &productMajor, uint8_t &productMinor,
                                 uint8_t &firmwareMajor,
                                 uint8_t &firmwareMinor) {
    uint8_t buffer[2] = {0, 0};
    if (!readEEprom(PRODUCT_VERSION, buffer, sizeof(buffer))) {
        return false;
    }
    productMinor = buffer[0];
    productMajor = buffer[1];

    if (!readEEprom(FIRMWARE_VERSION, buffer, sizeof(buffer))) {
        return false;
    }
    firmwareMinor = buffer[0];
    firmwareMajor = buffer[1];

    // An absent or unpowered module reads back all ones or all zeros on both
    // registers, which is how a wiring fault shows up here.
    if ((productMajor == 0xFF && productMinor == 0xFF) ||
        (productMajor == 0x00 && productMinor == 0x00)) {
        return false;
    }
    return true;
}
