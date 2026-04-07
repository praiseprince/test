#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

enum class PacketType : std::uint8_t {
    HANDSHAKE_REQUEST = 0x01,
    HANDSHAKE_ACK = 0x02,
    HANDSHAKE_FAIL = 0x03,
    TELEMETRY = 0x04,
    LARGE_FILE = 0x05,
    DISCONNECT = 0x06
};

#pragma pack(push, 1)
struct PacketHeader {
    PacketType packet_type;
    char aircraft_id[16];
    std::uint32_t sequence_number;
    std::uint32_t payload_size;
    std::uint32_t checksum;
};
#pragma pack(pop)

struct TelemetryPayload {
    float latitude;
    float longitude;
    float altitude;
    float speed;
    float heading;
};

struct LargeFilePayload {
    std::uint8_t* data;
    std::uint32_t size;

    LargeFilePayload() : data(nullptr), size(0) {}

    ~LargeFilePayload() {
        std::free(data);
        data = nullptr;
        size = 0;
    }

    LargeFilePayload(const LargeFilePayload&) = delete;
    LargeFilePayload& operator=(const LargeFilePayload&) = delete;

    LargeFilePayload(LargeFilePayload&& other) noexcept : data(other.data), size(other.size) {
        other.data = nullptr;
        other.size = 0;
    }

    LargeFilePayload& operator=(LargeFilePayload&& other) noexcept {
        if (this != &other) {
            std::free(data);
            data = other.data;
            size = other.size;
            other.data = nullptr;
            other.size = 0;
        }
        return *this;
    }
};

inline std::uint32_t computeChecksum(const std::uint8_t* data, std::uint32_t size) {
    std::uint32_t sum = 0;
    for (std::uint32_t i = 0; i < size; ++i) {
        sum += data[i];
    }
    return sum;
}

inline std::string packetTypeToString(PacketType type) {
    switch (type) {
        case PacketType::HANDSHAKE_REQUEST:
            return "HANDSHAKE_REQUEST";
        case PacketType::HANDSHAKE_ACK:
            return "HANDSHAKE_ACK";
        case PacketType::HANDSHAKE_FAIL:
            return "HANDSHAKE_FAIL";
        case PacketType::TELEMETRY:
            return "TELEMETRY";
        case PacketType::LARGE_FILE:
            return "LARGE_FILE";
        case PacketType::DISCONNECT:
            return "DISCONNECT";
    }
    return "UNKNOWN";
}

inline std::string extractAircraftId(const char aircraftId[16]) {
    std::size_t length = 0;
    while (length < 16 && aircraftId[length] != '\0') {
        ++length;
    }
    return std::string(aircraftId, aircraftId + length);
}

inline PacketHeader makeHeader(
    PacketType type,
    const std::string& aircraftId,
    std::uint32_t sequenceNumber,
    std::uint32_t payloadSize,
    std::uint32_t checksum) {
    PacketHeader header {};
    header.packet_type = type;
    std::memset(header.aircraft_id, 0, sizeof(header.aircraft_id));
    std::strncpy(header.aircraft_id, aircraftId.c_str(), sizeof(header.aircraft_id) - 1);
    header.sequence_number = sequenceNumber;
    header.payload_size = payloadSize;
    header.checksum = checksum;
    return header;
}
