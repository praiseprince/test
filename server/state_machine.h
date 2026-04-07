#pragma once

#include <string>

class StateMachine {
public:
    enum class State {
        DISCONNECTED,
        HANDSHAKE_PENDING,
        CONNECTED,
        TELEMETRY,
        LARGE_FILE_TRANSFER,
        FAULT
    };

    State getState() const;
    bool transition(State next);
    std::string stateToString(State s) const;

private:
    bool isAllowed(State from, State to) const;

    State current = State::DISCONNECTED;
};
