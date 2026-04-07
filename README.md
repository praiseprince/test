# Aviation Ground Control Communication System

CSCN74000 Group 4 sprint implementation in C++17 using a CLI aircraft client and a Dear ImGui ground-control server.

## Build

```bash
cmake -B build
cmake --build build
```

## Run

```bash
./build/ground_server
./build/aircraft_client
```

Optional server port:

```bash
./build/ground_server 5050
```

Optional client arguments:

```bash
./build/aircraft_client 127.0.0.1 5000 AC-001
```

If you omit the aircraft ID, the client now auto-assigns one and will retry with a different ID if the server rejects it as already in use:

```bash
./build/aircraft_client 127.0.0.1 5000
```

To connect to a custom server port, pass the same port to both programs:

```bash
./build/ground_server 5050
./build/aircraft_client 127.0.0.1 5050 AC-001
```

Headless mode is also available for environments that cannot open a GLFW window:

```bash
./build/ground_server --headless 5050
./build/aircraft_client 127.0.0.1 5050 AC-001
```

To simulate multiple aircraft, start one server and then launch multiple clients with distinct aircraft IDs:

```bash
./build/ground_server 5050
./build/aircraft_client 127.0.0.1 5050 AC-001
./build/aircraft_client 127.0.0.1 5050 AC-002
```

Each client must use a unique aircraft ID in the format `AC-001`. The server dashboard tracks aircraft independently, and the weather-map and disconnect controls apply to the currently selected aircraft only. The GUI also includes `Previous Flight` and `Next Flight` buttons so you can cycle through connected aircraft without clicking the list manually.

## Runtime Output Layout

- `runtime/logs/aircraft_comms/`: aircraft client communication logs.
- `runtime/logs/groundctrl_comms/`: ground server communication logs.
- `runtime/logs/blackbox/`: server fault-only black box logs.
- `runtime/bitmaps/generated/`: timestamped server-generated weather map BMPs, one file per send.
- `runtime/bitmaps/received/`: timestamped client-downloaded weather map BMPs, one file per receive.
- `runtime/ui/`: Dear ImGui runtime window-layout state such as `imgui.ini`.

## Repository Layout

- `common/`: shared packet definitions, socket helpers, and timestamped logging.
- `client/`: CLI aircraft client with handshake, telemetry, disconnect, and weather-map download flow.
- `server/`: concurrent socket/state-machine loop, weather-map generation, and Dear ImGui dashboard rendering.
- `third_party/imgui/`: bundled Dear ImGui sources and OpenGL/GLFW backends.

Logs are written under `runtime/logs/` with UTC timestamped filenames.

Each weather-map send writes a new timestamped BMP in `runtime/bitmaps/generated/`, and each receive writes a new timestamped BMP in `runtime/bitmaps/received/`. The filenames include the aircraft ID and transfer sequence so you can trace which generated file matches which received file.
