#include "logger.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

const std::filesystem::path kLogsRoot = "runtime/logs";

std::string makeTimestampPrefix() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
#ifdef _WIN32
    const int processId = _getpid();
#else
    const int processId = static_cast<int>(getpid());
#endif

    const std::tm* utcNow = std::gmtime(&nowTime);

    std::ostringstream out;
    out << std::put_time(utcNow, "%Y%m%d_%H%M%S")
        << '_'
        << std::setw(3) << std::setfill('0') << milliseconds
        << "_pid" << processId;
    return out.str();
}

std::string makeCommsLogName(const std::string& role, const std::string& prefix) {
    return prefix + "_" + role + "_comms.log";
}

std::string makeBlackBoxLogName(const std::string& prefix) {
    return prefix + "_blackbox.log";
}

std::filesystem::path logDirectoryForRole(const std::string& role) {
    if (role == "aircraft") {
        return kLogsRoot / "aircraft_comms";
    }
    return kLogsRoot / "groundctrl_comms";
}

} // namespace

Logger::Logger(const std::string& role) {
    const std::string prefix = makeTimestampPrefix();
    const std::filesystem::path commsDirectory = logDirectoryForRole(role);
    std::filesystem::create_directories(commsDirectory);
    commsLog.open((commsDirectory / makeCommsLogName(role, prefix)).string(), std::ios::out);
    if (role == "groundctrl") {
        const std::filesystem::path blackboxDirectory = kLogsRoot / "blackbox";
        std::filesystem::create_directories(blackboxDirectory);
        blackBoxLog.open((blackboxDirectory / makeBlackBoxLogName(prefix)).string(), std::ios::out);
    }
}

void Logger::logPacket(const std::string& direction, const PacketHeader& hdr) {
    std::lock_guard<std::mutex> lock(mutex);
    commsLog << "[" << timestampUtc() << "] "
             << "[" << direction << "] "
             << "[" << packetTypeToString(hdr.packet_type) << "] "
             << "Aircraft=" << extractAircraftId(hdr.aircraft_id) << ' '
             << "Seq=" << std::setw(4) << std::setfill('0') << hdr.sequence_number << ' '
             << "PayloadSize=" << hdr.payload_size << '\n';
    commsLog.flush();
}

void Logger::logFault(const std::string& cause, const std::string& state, std::uint32_t seq) {
    std::lock_guard<std::mutex> lock(mutex);
    std::ostringstream line;
    line << "[" << timestampUtc() << "] "
         << "[FAULT] "
         << "Cause=" << cause << ' '
         << "State=" << state << ' '
         << "Seq=" << std::setw(4) << std::setfill('0') << seq;

    commsLog << line.str() << '\n';
    commsLog.flush();

    if (blackBoxLog.is_open()) {
        blackBoxLog << line.str() << '\n';
        blackBoxLog.flush();
    }
}

void Logger::logInfo(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex);
    commsLog << "[" << timestampUtc() << "] "
             << "[INFO] "
             << message << '\n';
    commsLog.flush();
}

std::string Logger::timestampUtc() const {
    const std::time_t now = std::time(nullptr);
    const std::tm* utcNow = std::gmtime(&now);

    std::ostringstream out;
    out << std::put_time(utcNow, "%Y-%m-%d %H:%M:%S UTC");
    return out.str();
}
