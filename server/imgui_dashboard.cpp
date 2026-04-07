#include "imgui_dashboard.h"

#include <cstdio>
#include <string>

namespace {

const char* shortStateLabel(StateMachine::State state) {
    switch (state) {
        case StateMachine::State::DISCONNECTED:
            return "DISCONNECTED";
        case StateMachine::State::HANDSHAKE_PENDING:
            return "HANDSHAKE_PENDING";
        case StateMachine::State::CONNECTED:
            return "CONNECTED";
        case StateMachine::State::TELEMETRY:
            return "TELEMETRY";
        case StateMachine::State::LARGE_FILE_TRANSFER:
            return "LARGE_FILE_TRANSFER";
        case StateMachine::State::FAULT:
            return "FAULT";
    }
    return "UNKNOWN";
}

void renderTelemetryValue(const char* label, const char* value) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(120.0f);
    ImGui::TextUnformatted(value);
}

AircraftDashboardState* selectedAircraft(DashboardState& state) {
    if (state.selectedAircraftId.empty() && !state.aircraft.empty()) {
        state.selectedAircraftId = state.aircraft.begin()->first;
    }

    auto selected = state.aircraft.find(state.selectedAircraftId);
    if (selected != state.aircraft.end()) {
        return &selected->second;
    }

    if (!state.aircraft.empty()) {
        state.selectedAircraftId = state.aircraft.begin()->first;
        return &state.aircraft.begin()->second;
    }

    state.selectedAircraftId.clear();
    return nullptr;
}

void selectRelativeAircraft(DashboardState& state, int direction) {
    if (state.aircraft.empty()) {
        state.selectedAircraftId.clear();
        return;
    }

    auto selected = state.aircraft.find(state.selectedAircraftId);
    if (selected == state.aircraft.end()) {
        state.selectedAircraftId = state.aircraft.begin()->first;
        return;
    }

    if (direction > 0) {
        ++selected;
        if (selected == state.aircraft.end()) {
            selected = state.aircraft.begin();
        }
    } else {
        if (selected == state.aircraft.begin()) {
            selected = state.aircraft.end();
        }
        --selected;
    }

    state.selectedAircraftId = selected->first;
}

} // namespace

ImVec4 stateColor(StateMachine::State state) {
    switch (state) {
        case StateMachine::State::DISCONNECTED:
            return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        case StateMachine::State::HANDSHAKE_PENDING:
            return ImVec4(0.9f, 0.8f, 0.2f, 1.0f);
        case StateMachine::State::CONNECTED:
        case StateMachine::State::TELEMETRY:
            return ImVec4(0.2f, 0.8f, 0.3f, 1.0f);
        case StateMachine::State::LARGE_FILE_TRANSFER:
            return ImVec4(0.2f, 0.5f, 0.9f, 1.0f);
        case StateMachine::State::FAULT:
            return ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

void renderDashboard(DashboardState& state) {
    ImGui::SetNextWindowSize(ImVec2(1080.0f, 760.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Ground Control Operator Dashboard");

    ImGui::TextUnformatted("GROUND CONTROL OPERATOR DASHBOARD");
    ImGui::Separator();

    AircraftDashboardState* selected = selectedAircraft(state);
    const bool canControlSelected = selected != nullptr && selected->connected;

    ImGui::BeginChild("aircraft_panel", ImVec2(300.0f, 260.0f), true);
    ImGui::TextUnformatted("TRACKED AIRCRAFT");
    ImGui::Separator();
    ImGui::Text("Known Aircraft: %d", static_cast<int>(state.aircraft.size()));
    ImGui::BeginDisabled(state.aircraft.empty());
    if (ImGui::Button("Previous Flight", ImVec2(140.0f, 0.0f))) {
        selectRelativeAircraft(state, -1);
        selected = selectedAircraft(state);
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Flight", ImVec2(-1.0f, 0.0f))) {
        selectRelativeAircraft(state, 1);
        selected = selectedAircraft(state);
    }
    ImGui::EndDisabled();
    ImGui::Spacing();

    for (auto& [aircraftId, aircraft] : state.aircraft) {
        const bool isSelected = aircraftId == state.selectedAircraftId;
        std::string label = aircraftId + " [" + shortStateLabel(aircraft.state) + "]";
        if (ImGui::Selectable(label.c_str(), isSelected)) {
            state.selectedAircraftId = aircraftId;
            selected = &aircraft;
        }
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!canControlSelected);
    if (ImGui::Button("Send Weather Map to Selected", ImVec2(-1.0f, 0.0f)) && selected != nullptr) {
        state.weatherRequestAircraftId = selected->aircraftId;
    }
    if (ImGui::Button("Disconnect Selected", ImVec2(-1.0f, 0.0f)) && selected != nullptr) {
        state.disconnectRequestAircraftId = selected->aircraftId;
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("telemetry_panel", ImVec2(0.0f, 260.0f), true);
    ImGui::TextUnformatted("SELECTED AIRCRAFT");
    ImGui::Separator();

    if (selected == nullptr) {
        ImGui::TextUnformatted("No aircraft connected yet.");
    } else {
        ImGui::Text("Viewing %s", selected->aircraftId.c_str());
        renderTelemetryValue("Aircraft ID", selected->aircraftId.c_str());
        ImGui::Text("State:");
        ImGui::SameLine();
        ImGui::TextColored(stateColor(selected->state), "%s", shortStateLabel(selected->state));

        char buffer[64] {};
        if (selected->telemetryValid) {
            std::snprintf(buffer, sizeof(buffer), "%.4f", selected->telemetry.latitude);
            renderTelemetryValue("Latitude", buffer);
            std::snprintf(buffer, sizeof(buffer), "%.4f", selected->telemetry.longitude);
            renderTelemetryValue("Longitude", buffer);
            std::snprintf(buffer, sizeof(buffer), "%.0f ft", selected->telemetry.altitude);
            renderTelemetryValue("Altitude", buffer);
            std::snprintf(buffer, sizeof(buffer), "%.1f kts", selected->telemetry.speed);
            renderTelemetryValue("Speed", buffer);
            std::snprintf(buffer, sizeof(buffer), "%.0f deg", selected->telemetry.heading);
            renderTelemetryValue("Heading", buffer);
        } else {
            renderTelemetryValue("Latitude", "--");
            renderTelemetryValue("Longitude", "--");
            renderTelemetryValue("Altitude", "--");
            renderTelemetryValue("Speed", "--");
            renderTelemetryValue("Heading", "--");
        }

        if (!selected->alertMessage.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.95f, 0.25f, 0.25f, 1.0f), "Alert: %s", selected->alertMessage.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::BeginChild("log_panel", ImVec2(0.0f, 0.0f), true);
    ImGui::TextUnformatted("COMMUNICATION LOG (last 20 entries)");
    ImGui::Separator();
    for (const auto& entry : state.recentLogEntries) {
        ImGui::TextWrapped("%s", entry.c_str());
    }
    ImGui::EndChild();

    ImGui::End();
}
