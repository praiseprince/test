#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

#include "packet.h"

class Logger {
public:
    explicit Logger(const std::string& role);

    void logPacket(const std::string& direction, const PacketHeader& hdr);
    void logFault(const std::string& cause, const std::string& state, std::uint32_t seq);
    void logInfo(const std::string& message);

private:
    std::string timestampUtc() const;

    std::ofstream commsLog;
    std::ofstream blackBoxLog;
    mutable std::mutex mutex;
};
