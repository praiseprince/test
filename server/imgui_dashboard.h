#pragma once

#include <deque>
#include <map>
#include <string>

#include "imgui.h"

#include "packet.h"
#include "state_machine.h"

struct AircraftDashboardState {
    std::string aircraftId = "N/A";
    StateMachine::State state = StateMachine::State::DISCONNECTED;
    bool connected = false;
    bool telemetryValid = false;
    TelemetryPayload telemetry {};
    std::string alertMessage;
};

struct DashboardState {
    std::map<std::string, AircraftDashboardState> aircraft;
    std::string selectedAircraftId;
    std::deque<std::string> recentLogEntries;
    std::string weatherRequestAircraftId;
    std::string disconnectRequestAircraftId;
};

ImVec4 stateColor(StateMachine::State state);
void renderDashboard(DashboardState& state);
