#include "state_machine.h"

#include <iostream>

StateMachine::State StateMachine::getState() const {
    return current;
}

bool StateMachine::transition(State next) {
    if (!isAllowed(current, next)) {
        std::cerr << "[StateMachine] Illegal transition from "
                  << stateToString(current) << " to " << stateToString(next) << '\n';
        return false;
    }
    current = next;
    return true;
}

std::string StateMachine::stateToString(State s) const {
    switch (s) {
        case State::DISCONNECTED:
            return "STATE_DISCONNECTED";
        case State::HANDSHAKE_PENDING:
            return "STATE_HANDSHAKE_PENDING";
        case State::CONNECTED:
            return "STATE_CONNECTED";
        case State::TELEMETRY:
            return "STATE_TELEMETRY";
        case State::LARGE_FILE_TRANSFER:
            return "STATE_LARGE_FILE_TRANSFER";
        case State::FAULT:
            return "STATE_FAULT";
    }
    return "STATE_UNKNOWN";
}

bool StateMachine::isAllowed(State from, State to) const {
    if (from == to) {
        return true;
    }

    switch (from) {
        case State::DISCONNECTED:
            return to == State::HANDSHAKE_PENDING;
        case State::HANDSHAKE_PENDING:
            return to == State::CONNECTED || to == State::DISCONNECTED || to == State::FAULT;
        case State::CONNECTED:
            return to == State::TELEMETRY || to == State::LARGE_FILE_TRANSFER || to == State::DISCONNECTED ||
                   to == State::FAULT;
        case State::TELEMETRY:
            return to == State::CONNECTED || to == State::FAULT;
        case State::LARGE_FILE_TRANSFER:
            return to == State::CONNECTED || to == State::FAULT;
        case State::FAULT:
            return to == State::DISCONNECTED;
    }
    return false;
}
