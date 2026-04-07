#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_opengl3_loader.h"

#include "imgui_dashboard.h"
#include "logger.h"
#include "packet.h"
#include "socket_utils.h"
#include "state_machine.h"

namespace {

constexpr std::uint16_t kDefaultPort = 5000;
constexpr std::size_t kChunkSize = 4096;
const std::string kGeneratedWeatherMapDirectory = "runtime/bitmaps/generated";
const std::string kImguiIniPath = "runtime/ui/imgui.ini";

struct SharedServerState {
    std::mutex mutex;
    DashboardState dashboard;
};

struct ServerOptions {
    std::uint16_t listenPort = kDefaultPort;
    bool headless = false;
};

struct ClientSession {
    SocketHandle socket = INVALID_SOCK;
    std::string aircraftId;
    StateMachine stateMachine;
    TelemetryPayload telemetry {};
    bool telemetryValid = false;
    std::uint32_t serverSequence = 1;
    std::uint32_t lastClientSequence = 0;
    std::chrono::steady_clock::time_point lastPacketTime = std::chrono::steady_clock::now();
};

using SessionMap = std::map<SocketHandle, ClientSession>;

std::string timeStampHmsUtc() {
    const std::time_t now = std::time(nullptr);
    const std::tm* utcNow = std::gmtime(&now);
    std::ostringstream out;
    out << std::put_time(utcNow, "%H:%M:%S");
    return out.str();
}

std::string fileTimestampUtc() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    const std::tm* utcNow = std::gmtime(&nowTime);

    std::ostringstream out;
    out << std::put_time(utcNow, "%Y%m%d_%H%M%S")
        << '_'
        << std::setw(3) << std::setfill('0') << milliseconds;
    return out.str();
}

std::string formatSequence(std::uint32_t sequence) {
    std::ostringstream out;
    out << "seq" << std::setw(4) << std::setfill('0') << sequence;
    return out.str();
}

void appendDashboardLog(SharedServerState& sharedState, const std::string& entry) {
    std::lock_guard<std::mutex> lock(sharedState.mutex);
    sharedState.dashboard.recentLogEntries.push_front(entry);
    if (sharedState.dashboard.recentLogEntries.size() > 20) {
        sharedState.dashboard.recentLogEntries.pop_back();
    }
}

std::string compactPacketLog(const std::string& direction, const PacketHeader& header) {
    std::ostringstream line;
    line << "[" << timeStampHmsUtc() << "] "
         << direction << ' '
         << packetTypeToString(header.packet_type)
         << " Seq=" << header.sequence_number
         << ' ' << extractAircraftId(header.aircraft_id);
    return line.str();
}

void updateDashboardAircraft(
    SharedServerState& sharedState,
    const ClientSession& session,
    const std::string& alertMessage = std::string()) {
    if (session.aircraftId.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(sharedState.mutex);
    auto& aircraft = sharedState.dashboard.aircraft[session.aircraftId];
    aircraft.aircraftId = session.aircraftId;
    aircraft.state = session.stateMachine.getState();
    aircraft.connected =
        session.stateMachine.getState() != StateMachine::State::DISCONNECTED &&
        session.stateMachine.getState() != StateMachine::State::FAULT;
    aircraft.telemetry = session.telemetry;
    aircraft.telemetryValid = session.telemetryValid;
    aircraft.alertMessage = alertMessage;

    if (sharedState.dashboard.selectedAircraftId.empty()) {
        sharedState.dashboard.selectedAircraftId = session.aircraftId;
    }
}

std::string consumeWeatherRequest(SharedServerState& sharedState) {
    std::lock_guard<std::mutex> lock(sharedState.mutex);
    std::string aircraftId = sharedState.dashboard.weatherRequestAircraftId;
    sharedState.dashboard.weatherRequestAircraftId.clear();
    return aircraftId;
}

std::string consumeDisconnectRequest(SharedServerState& sharedState) {
    std::lock_guard<std::mutex> lock(sharedState.mutex);
    std::string aircraftId = sharedState.dashboard.disconnectRequestAircraftId;
    sharedState.dashboard.disconnectRequestAircraftId.clear();
    return aircraftId;
}

bool validateAircraftId(const std::string& aircraftId) {
    static const std::regex pattern("^AC-[0-9]{3}$");
    return std::regex_match(aircraftId, pattern);
}

bool sendPacket(SocketHandle socketHandle, const PacketHeader& header, const std::uint8_t* payload) {
    if (!sendAll(socketHandle, &header, sizeof(header))) {
        return false;
    }
    if (header.payload_size > 0) {
        return sendAll(socketHandle, payload, header.payload_size);
    }
    return true;
}

std::string generatedWeatherMapPathFor(const std::string& aircraftId, std::uint32_t transferSequence) {
    return kGeneratedWeatherMapDirectory + "/" + fileTimestampUtc() + "_" + aircraftId + "_" +
           formatSequence(transferSequence) + "_weather_map.bmp";
}

std::uint32_t aircraftSeed(const std::string& aircraftId) {
    std::uint32_t seed = 2166136261u;
    for (unsigned char ch : aircraftId) {
        seed ^= ch;
        seed *= 16777619u;
    }
    return seed;
}

std::uint8_t clampChannel(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void generateWeatherMap(const std::string& path, const ClientSession& session, std::uint32_t transferSequence) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    constexpr int width = 1024;
    constexpr int height = 1024;
    constexpr int bytesPerPixel = 3;
    constexpr std::uint32_t pixelDataSize = width * height * bytesPerPixel;
    constexpr std::uint32_t fileSize = 54 + pixelDataSize;

    std::array<std::uint8_t, 54> header {};
    header[0] = 'B';
    header[1] = 'M';
    std::memcpy(&header[2], &fileSize, sizeof(fileSize));
    const std::uint32_t pixelOffset = 54;
    std::memcpy(&header[10], &pixelOffset, sizeof(pixelOffset));
    const std::uint32_t dibSize = 40;
    std::memcpy(&header[14], &dibSize, sizeof(dibSize));
    std::int32_t signedWidth = width;
    std::int32_t signedHeight = height;
    std::memcpy(&header[18], &signedWidth, sizeof(signedWidth));
    std::memcpy(&header[22], &signedHeight, sizeof(signedHeight));
    const std::uint16_t planes = 1;
    const std::uint16_t bitCount = 24;
    std::memcpy(&header[26], &planes, sizeof(planes));
    std::memcpy(&header[28], &bitCount, sizeof(bitCount));
    std::memcpy(&header[34], &pixelDataSize, sizeof(pixelDataSize));

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));

    const std::uint32_t seed = aircraftSeed(session.aircraftId) ^ (transferSequence * 2246822519u);
    int stormCenterX = static_cast<int>(seed % width);
    int stormCenterY = static_cast<int>((seed >> 12) % height);
    int stormRadius = 160 + static_cast<int>(seed % 120);
    int frontOffset = static_cast<int>((seed >> 20) % 220);
    int altitudeBand = 180 + static_cast<int>((seed >> 8) % 320);

    if (session.telemetryValid) {
        stormCenterX = std::clamp(
            static_cast<int>(((session.telemetry.longitude + 180.0f) / 360.0f) * static_cast<float>(width - 1)),
            0,
            width - 1);
        stormCenterY = std::clamp(
            height - 1 -
                static_cast<int>(((session.telemetry.latitude + 90.0f) / 180.0f) * static_cast<float>(height - 1)),
            0,
            height - 1);
        stormRadius = std::clamp(150 + static_cast<int>(session.telemetry.speed * 0.25f), 150, 320);
        frontOffset = static_cast<int>(session.telemetry.heading) % 220;
        altitudeBand = std::clamp(static_cast<int>(session.telemetry.altitude / 120.0f), 120, height - 1);
    }

    const int stormFalloff = std::max(1, (stormRadius * stormRadius) / 255);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * bytesPerPixel);
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(x) * bytesPerPixel;

            const int dx = x - stormCenterX;
            const int dy = y - stormCenterY;
            const int stormIntensity = std::max(0, 255 - ((dx * dx + dy * dy) / stormFalloff));
            const int frontBand =
                std::max(0, 120 - std::abs((((x + y) + frontOffset) % 240) - 120));
            const int altitudeStripe =
                std::max(0, 90 - std::abs((((y * 2) + altitudeBand) % 180) - 90));
            const int turbulence =
                std::max(0, 75 - std::abs((((x ^ y) + static_cast<int>(seed & 0xFFu)) % 150) - 75));

            const int blueBase = 85 + ((x * 70) / width) + static_cast<int>((seed >> 16) & 0x1F);
            const int greenBase = 55 + ((y * 65) / height) + static_cast<int>((seed >> 8) & 0x1F);
            const int redBase = 20 + (((x + y) * 30) / (width + height)) + static_cast<int>(seed & 0x1F);

            int blue = blueBase + (stormIntensity / 2) + (frontBand / 5);
            int green = greenBase + (stormIntensity / 4) + (altitudeStripe / 3);
            int red = redBase + (stormIntensity / 7) + (frontBand / 3) + (turbulence / 4);

            if (stormIntensity > 205 && ((x + y + static_cast<int>(seed)) % 97) == 0) {
                red = 250;
                green = 245;
                blue = 220;
            }

            row[offset + 0] = clampChannel(blue);
            row[offset + 1] = clampChannel(green);
            row[offset + 2] = clampChannel(red);
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
}

bool aircraftIdInUse(const SessionMap& sessions, SocketHandle currentSocket, const std::string& aircraftId) {
    for (const auto& [socketHandle, session] : sessions) {
        if (socketHandle == currentSocket) {
            continue;
        }
        if (session.aircraftId == aircraftId && session.stateMachine.getState() != StateMachine::State::DISCONNECTED) {
            return true;
        }
    }
    return false;
}

void closeClientSession(ClientSession& session) {
    shutdownSocket(session.socket);
    closeSocket(session.socket);
    session.socket = INVALID_SOCK;
}

void handleFault(
    ClientSession& session,
    Logger& logger,
    SharedServerState& sharedState,
    const std::string& cause) {
    const StateMachine::State currentState = session.stateMachine.getState();
    if (currentState != StateMachine::State::FAULT) {
        session.stateMachine.transition(StateMachine::State::FAULT);
    }
    logger.logFault(cause, session.stateMachine.stateToString(currentState), session.lastClientSequence);
    appendDashboardLog(
        sharedState,
        "[" + timeStampHmsUtc() + "] FAULT " +
            (session.aircraftId.empty() ? std::string("UNKNOWN") : session.aircraftId + " ") + cause);
    updateDashboardAircraft(sharedState, session, cause);
    session.stateMachine.transition(StateMachine::State::DISCONNECTED);
    updateDashboardAircraft(sharedState, session);
}

bool sendWeatherMap(ClientSession& session, Logger& logger, SharedServerState& sharedState) {
    if (session.stateMachine.getState() == StateMachine::State::TELEMETRY) {
        session.stateMachine.transition(StateMachine::State::CONNECTED);
    }
    if (!session.stateMachine.transition(StateMachine::State::LARGE_FILE_TRANSFER)) {
        handleFault(session, logger, sharedState, "IllegalFileTransferTransition");
        return false;
    }
    updateDashboardAircraft(sharedState, session);

    const std::uint32_t transferSequence = session.serverSequence;
    const std::string weatherMapPath = generatedWeatherMapPathFor(session.aircraftId, transferSequence);
    generateWeatherMap(weatherMapPath, session, transferSequence);

    std::ifstream file(weatherMapPath, std::ios::binary | std::ios::ate);
    if (!file) {
        handleFault(session, logger, sharedState, "WeatherMapMissing");
        return false;
    }

    const auto size = static_cast<std::uint32_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    LargeFilePayload payload;
    payload.data = static_cast<std::uint8_t*>(std::malloc(size));
    payload.size = size;
    if (!payload.data || !file.read(reinterpret_cast<char*>(payload.data), size)) {
        handleFault(session, logger, sharedState, "WeatherMapReadFailure");
        return false;
    }

    PacketHeader header = makeHeader(
        PacketType::LARGE_FILE,
        session.aircraftId,
        transferSequence,
        payload.size,
        computeChecksum(payload.data, payload.size));
    ++session.serverSequence;

    if (!sendAll(session.socket, &header, sizeof(header))) {
        handleFault(session, logger, sharedState, "FileHeaderSendFailure");
        return false;
    }

    for (std::uint32_t offset = 0; offset < payload.size; offset += static_cast<std::uint32_t>(kChunkSize)) {
        const std::uint32_t chunkSize =
            std::min<std::uint32_t>(static_cast<std::uint32_t>(kChunkSize), payload.size - offset);
        if (!sendAll(session.socket, payload.data + offset, chunkSize)) {
            handleFault(session, logger, sharedState, "FileChunkSendFailure");
            return false;
        }
    }

    logger.logPacket("TX", header);
    logger.logInfo("Generated weather map: " + weatherMapPath);
    appendDashboardLog(sharedState, compactPacketLog("TX", header));
    appendDashboardLog(
        sharedState,
        "[" + timeStampHmsUtc() + "] MAP " + session.aircraftId + " " +
            std::filesystem::path(weatherMapPath).filename().string());
    session.stateMachine.transition(StateMachine::State::CONNECTED);
    updateDashboardAircraft(sharedState, session);
    return true;
}

bool receivePacketPayload(SocketHandle clientSocket, const PacketHeader& header, std::vector<std::uint8_t>& payload) {
    payload.assign(header.payload_size, 0);
    if (header.payload_size == 0) {
        return true;
    }
    return recvAll(clientSocket, payload.data(), header.payload_size);
}

SessionMap::iterator findSessionByAircraftId(SessionMap& sessions, const std::string& aircraftId) {
    for (auto it = sessions.begin(); it != sessions.end(); ++it) {
        if (it->second.aircraftId == aircraftId) {
            return it;
        }
    }
    return sessions.end();
}

bool processHandshake(
    SessionMap& sessions,
    SessionMap::iterator sessionIt,
    const PacketHeader& header,
    Logger& logger,
    SharedServerState& sharedState) {
    ClientSession& session = sessionIt->second;
    const std::string aircraftId = extractAircraftId(header.aircraft_id);

    const bool validRequest =
        header.packet_type == PacketType::HANDSHAKE_REQUEST &&
        header.payload_size == 0 &&
        validateAircraftId(aircraftId) &&
        !aircraftIdInUse(sessions, session.socket, aircraftId);

    if (!validRequest) {
        PacketHeader failHeader = makeHeader(PacketType::HANDSHAKE_FAIL, aircraftId, header.sequence_number, 0, 0);
        sendPacket(session.socket, failHeader, nullptr);
        logger.logPacket("TX", failHeader);
        appendDashboardLog(sharedState, compactPacketLog("TX", failHeader));
        return false;
    }

    session.aircraftId = aircraftId;
    session.lastClientSequence = header.sequence_number;
    session.lastPacketTime = std::chrono::steady_clock::now();

    PacketHeader ackHeader = makeHeader(PacketType::HANDSHAKE_ACK, aircraftId, header.sequence_number, 0, 0);
    if (!sendPacket(session.socket, ackHeader, nullptr)) {
        handleFault(session, logger, sharedState, "HandshakeAckSendFailure");
        return false;
    }

    logger.logPacket("TX", ackHeader);
    appendDashboardLog(sharedState, compactPacketLog("TX", ackHeader));
    session.stateMachine.transition(StateMachine::State::CONNECTED);
    updateDashboardAircraft(sharedState, session);
    return true;
}

bool processReadableSession(
    SessionMap& sessions,
    SessionMap::iterator sessionIt,
    Logger& logger,
    SharedServerState& sharedState) {
    ClientSession& session = sessionIt->second;

    PacketHeader header {};
    if (!recvAll(session.socket, &header, sizeof(header))) {
        handleFault(
            session,
            logger,
            sharedState,
            session.stateMachine.getState() == StateMachine::State::HANDSHAKE_PENDING ? "HandshakeReceiveFailure"
                                                                                      : "ReceiveFailure");
        return false;
    }

    logger.logPacket("RX", header);
    appendDashboardLog(sharedState, compactPacketLog("RX", header));

    if (session.stateMachine.getState() == StateMachine::State::HANDSHAKE_PENDING) {
        return processHandshake(sessions, sessionIt, header, logger, sharedState);
    }

    session.lastPacketTime = std::chrono::steady_clock::now();
    session.lastClientSequence = header.sequence_number;

    std::vector<std::uint8_t> payload;
    if (!receivePacketPayload(session.socket, header, payload)) {
        handleFault(session, logger, sharedState, "PayloadReceiveFailure");
        return false;
    }

    if (computeChecksum(payload.data(), header.payload_size) != header.checksum) {
        handleFault(session, logger, sharedState, "ChecksumMismatch");
        return false;
    }

    switch (header.packet_type) {
        case PacketType::TELEMETRY: {
            if (header.payload_size != sizeof(TelemetryPayload)) {
                handleFault(session, logger, sharedState, "TelemetrySizeMismatch");
                return false;
            }
            if (session.stateMachine.getState() == StateMachine::State::CONNECTED) {
                session.stateMachine.transition(StateMachine::State::TELEMETRY);
            }
            if (session.stateMachine.getState() != StateMachine::State::TELEMETRY) {
                handleFault(session, logger, sharedState, "IllegalTelemetryState");
                return false;
            }
            std::memcpy(&session.telemetry, payload.data(), sizeof(session.telemetry));
            session.telemetryValid = true;
            updateDashboardAircraft(sharedState, session);
            return true;
        }
        case PacketType::LARGE_FILE:
            if (header.payload_size != 0) {
                handleFault(session, logger, sharedState, "LargeFileRequestPayloadUnexpected");
                return false;
            }
            return sendWeatherMap(session, logger, sharedState);
        case PacketType::DISCONNECT:
            if (session.stateMachine.getState() == StateMachine::State::TELEMETRY) {
                session.stateMachine.transition(StateMachine::State::CONNECTED);
            }
            session.stateMachine.transition(StateMachine::State::DISCONNECTED);
            updateDashboardAircraft(sharedState, session);
            return false;
        default:
            handleFault(session, logger, sharedState, "UnexpectedPacketType");
            return false;
    }
}

bool sessionTimedOut(const ClientSession& session, std::chrono::steady_clock::time_point now) {
    const StateMachine::State state = session.stateMachine.getState();
    if (state == StateMachine::State::DISCONNECTED || state == StateMachine::State::FAULT) {
        return false;
    }

    const long timeoutSeconds = state == StateMachine::State::TELEMETRY ? 3L : 5L;
    return std::chrono::duration_cast<std::chrono::seconds>(now - session.lastPacketTime).count() >= timeoutSeconds;
}

void removeSession(
    SessionMap& sessions,
    SocketHandle socketHandle,
    std::atomic<bool>& running) {
    const auto sessionIt = sessions.find(socketHandle);
    if (sessionIt == sessions.end()) {
        return;
    }

    closeClientSession(sessionIt->second);
    sessions.erase(sessionIt);
}

void serverThreadMain(
    SocketHandle listenSocket,
    std::atomic<bool>& running,
    Logger& logger,
    SharedServerState& sharedState) {
    SessionMap sessions;

    while (running.load()) {
        const std::string weatherRequestAircraftId = consumeWeatherRequest(sharedState);
        if (!weatherRequestAircraftId.empty()) {
            const auto sessionIt = findSessionByAircraftId(sessions, weatherRequestAircraftId);
            if (sessionIt != sessions.end()) {
                if (!sendWeatherMap(sessionIt->second, logger, sharedState)) {
                    removeSession(sessions, sessionIt->first, running);
                    continue;
                }
            }
        }

        const std::string disconnectRequestAircraftId = consumeDisconnectRequest(sharedState);
        if (!disconnectRequestAircraftId.empty()) {
            const auto sessionIt = findSessionByAircraftId(sessions, disconnectRequestAircraftId);
            if (sessionIt != sessions.end()) {
                ClientSession& session = sessionIt->second;
                if (session.stateMachine.getState() == StateMachine::State::TELEMETRY) {
                    session.stateMachine.transition(StateMachine::State::CONNECTED);
                }
                PacketHeader disconnectHeader =
                    makeHeader(PacketType::DISCONNECT, session.aircraftId, session.serverSequence++, 0, 0);
                if (sendPacket(session.socket, disconnectHeader, nullptr)) {
                    logger.logPacket("TX", disconnectHeader);
                    appendDashboardLog(sharedState, compactPacketLog("TX", disconnectHeader));
                    session.stateMachine.transition(StateMachine::State::DISCONNECTED);
                    updateDashboardAircraft(sharedState, session);
                } else {
                    handleFault(session, logger, sharedState, "DisconnectSendFailure");
                }
                removeSession(sessions, sessionIt->first, running);
                continue;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        std::vector<SocketHandle> timedOutSessions;
        for (const auto& [socketHandle, session] : sessions) {
            if (sessionTimedOut(session, now)) {
                timedOutSessions.push_back(socketHandle);
            }
        }
        for (SocketHandle socketHandle : timedOutSessions) {
            auto sessionIt = sessions.find(socketHandle);
            if (sessionIt == sessions.end()) {
                continue;
            }
            handleFault(sessionIt->second, logger, sharedState, "SocketTimeout");
            removeSession(sessions, socketHandle, running);
        }
        if (!running.load()) {
            break;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);

        SocketHandle maxSocket = listenSocket;
        for (const auto& [socketHandle, session] : sessions) {
            if (session.socket != INVALID_SOCK) {
                FD_SET(session.socket, &readSet);
                if (session.socket > maxSocket) {
                    maxSocket = session.socket;
                }
            }
        }

        timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 250000;

#ifdef _WIN32
        const int selectResult = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int selectResult = select(maxSocket + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (selectResult < 0) {
            if (running.load()) {
                logger.logInfo(socketErrorString("select() failed"));
            }
            continue;
        }
        if (selectResult == 0) {
            continue;
        }

        if (FD_ISSET(listenSocket, &readSet)) {
            sockaddr_in clientAddress {};
            SockLenType clientLength = static_cast<SockLenType>(sizeof(clientAddress));
            const SocketHandle clientSocket =
                accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
            if (clientSocket != INVALID_SOCK) {
                ClientSession session;
                session.socket = clientSocket;
                session.lastPacketTime = std::chrono::steady_clock::now();
                session.stateMachine.transition(StateMachine::State::HANDSHAKE_PENDING);
                sessions.emplace(clientSocket, std::move(session));
            } else if (running.load()) {
                logger.logInfo(socketErrorString("accept() failed"));
            }
        }

        std::vector<SocketHandle> readySessions;
        for (const auto& [socketHandle, session] : sessions) {
            if (FD_ISSET(socketHandle, &readSet)) {
                readySessions.push_back(socketHandle);
            }
        }

        for (SocketHandle socketHandle : readySessions) {
            auto sessionIt = sessions.find(socketHandle);
            if (sessionIt == sessions.end()) {
                continue;
            }

            if (!processReadableSession(sessions, sessionIt, logger, sharedState)) {
                removeSession(sessions, socketHandle, running);
                if (!running.load()) {
                    break;
                }
            }
        }
    }

    for (auto& [socketHandle, session] : sessions) {
        closeClientSession(session);
    }
}

std::optional<ServerOptions> parseServerOptions(int argc, char* argv[]) {
    ServerOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless") {
            options.headless = true;
            continue;
        }

        try {
            const int parsedPort = std::stoi(arg);
            if (parsedPort < 1 || parsedPort > 65535) {
                std::cerr << "Invalid port. Please choose a value between 1 and 65535.\n";
                return std::nullopt;
            }
            options.listenPort = static_cast<std::uint16_t>(parsedPort);
        } catch (const std::exception&) {
            std::cerr << "Invalid argument. Usage: ./ground_server [port] [--headless]\n";
            return std::nullopt;
        }
    }

    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::optional<ServerOptions> parsedOptions = parseServerOptions(argc, argv);
    if (!parsedOptions.has_value()) {
        return 1;
    }
    const ServerOptions options = *parsedOptions;

    initSockets();
    Logger logger("groundctrl");

    SocketHandle listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCK) {
        std::cerr << "Unable to create server socket.\n";
        cleanupSockets();
        return 1;
    }

    setReuseAddress(listenSocket);

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.listenPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCK_ERR) {
        std::cerr << "Unable to bind server socket on port " << options.listenPort << ".\n";
        closeSocket(listenSocket);
        cleanupSockets();
        return 1;
    }

    if (listen(listenSocket, 8) == SOCK_ERR) {
        std::cerr << "Unable to listen on server socket on port " << options.listenPort << ".\n";
        closeSocket(listenSocket);
        cleanupSockets();
        return 1;
    }

    SharedServerState sharedState;
    std::atomic<bool> running {true};

    if (options.headless) {
        std::cout << "Ground server running in headless mode on port " << options.listenPort << ".\n";
        std::thread worker(
            serverThreadMain,
            listenSocket,
            std::ref(running),
            std::ref(logger),
            std::ref(sharedState));
        if (worker.joinable()) {
            worker.join();
        }
        shutdownSocket(listenSocket);
        closeSocket(listenSocket);
        cleanupSockets();
        return 0;
    }

    if (!glfwInit()) {
        std::cerr << "Unable to initialize GLFW.\n";
        closeSocket(listenSocket);
        cleanupSockets();
        return 1;
    }

    const char* glslVersion = "#version 130";
#ifdef __APPLE__
    glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    GLFWwindow* window = glfwCreateWindow(1080, 760, "Ground Control - CSCN74000", nullptr, nullptr);
    if (window == nullptr) {
        std::cerr << "Unable to create GLFW window.\n";
        glfwTerminate();
        closeSocket(listenSocket);
        cleanupSockets();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    std::filesystem::create_directories(std::filesystem::path(kImguiIniPath).parent_path());
    ImGui::GetIO().IniFilename = kImguiIniPath.c_str();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    std::thread worker(
        serverThreadMain,
        listenSocket,
        std::ref(running),
        std::ref(logger),
        std::ref(sharedState));

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        {
            std::lock_guard<std::mutex> lock(sharedState.mutex);
            renderDashboard(sharedState.dashboard);
        }

        ImGui::Render();
        int displayWidth = 0;
        int displayHeight = 0;
        glfwGetFramebufferSize(window, &displayWidth, &displayHeight);
        glViewport(0, 0, displayWidth, displayHeight);
        glClearColor(0.09f, 0.11f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    running.store(false);
    shutdownSocket(listenSocket);
    closeSocket(listenSocket);
    if (worker.joinable()) {
        worker.join();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    cleanupSockets();
    return 0;
}
