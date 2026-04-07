#!/usr/bin/env python3

from __future__ import annotations

import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = ROOT / "build"
RUNTIME_DIR = ROOT / "runtime"
HOST = "127.0.0.1"
STARTUP_TIMEOUT_SECONDS = 20
STARTUP_GRACE_SECONDS = 1.5
CLIENT_TIMEOUT_SECONDS = 40
SERVER_SHUTDOWN_TIMEOUT_SECONDS = 10
BUILD_CONFIG = os.environ.get("CI_BUILD_CONFIG", "").strip()
PREFER_GUI = os.environ.get("CI_PREFER_GUI", "1").strip() != "0"
USE_XVFB = os.environ.get("CI_USE_XVFB", "").strip() == "1"


def candidate_executable_paths(name: str) -> list[Path]:
    suffix = ".exe" if os.name == "nt" else ""
    candidates: list[Path] = []

    if BUILD_CONFIG:
        candidates.append(BUILD_DIR / BUILD_CONFIG / f"{name}{suffix}")

    candidates.extend(
        [
            BUILD_DIR / f"{name}{suffix}",
            BUILD_DIR / "Release" / f"{name}{suffix}",
            BUILD_DIR / "Debug" / f"{name}{suffix}",
            BUILD_DIR / "RelWithDebInfo" / f"{name}{suffix}",
            BUILD_DIR / "MinSizeRel" / f"{name}{suffix}",
        ]
    )
    return candidates


def find_executable(name: str) -> Path:
    for candidate in candidate_executable_paths(name):
        if candidate.exists():
            return candidate
    searched = "\n".join(f"- {path}" for path in candidate_executable_paths(name))
    raise FileNotFoundError(f"Unable to locate {name}. Searched:\n{searched}")


def choose_port() -> int:
    configured_port = os.environ.get("CI_TEST_PORT", "").strip()
    if configured_port:
        return int(configured_port)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind((HOST, 0))
        return int(probe.getsockname()[1])


def wait_for_server(server_process: subprocess.Popen[str]) -> None:
    deadline = time.time() + STARTUP_TIMEOUT_SECONDS
    grace_deadline = time.time() + STARTUP_GRACE_SECONDS
    while time.time() < deadline:
        if server_process.poll() is not None:
            output, _ = server_process.communicate(timeout=1)
            raise RuntimeError(f"Server exited before becoming reachable.\n{output}")
        time.sleep(0.2)
        if time.time() >= grace_deadline:
            return

    raise TimeoutError("Timed out waiting for the headless server to stay alive during startup.")


def latest_log_text(directory: Path) -> str:
    log_files = sorted(directory.glob("*.log"))
    if not log_files:
        raise FileNotFoundError(f"No log files found in {directory}")
    return log_files[-1].read_text(encoding="utf-8", errors="replace")


def verify_logs() -> None:
    aircraft_log = latest_log_text(RUNTIME_DIR / "logs" / "aircraft_comms")
    server_log = latest_log_text(RUNTIME_DIR / "logs" / "groundctrl_comms")
    blackbox_log = latest_log_text(RUNTIME_DIR / "logs" / "blackbox")

    if "HANDSHAKE_ACK" not in aircraft_log:
        raise AssertionError("Aircraft communication log does not contain a handshake acknowledgement.")

    if "HANDSHAKE_REQUEST" not in server_log or "HANDSHAKE_ACK" not in server_log:
        raise AssertionError("Server communication log does not contain the expected handshake flow.")

    if "[FAULT]" in blackbox_log:
        raise AssertionError("Black box log contains fault entries during verification.")


def server_command(server_executable: Path, port: int, mode: str) -> list[str]:
    command: list[str] = []
    if mode == "gui" and USE_XVFB:
        command.extend(["xvfb-run", "-a"])

    command.append(str(server_executable))
    if mode == "headless":
        command.append("--headless")
    command.append(str(port))
    return command


def terminate_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.communicate(timeout=SERVER_SHUTDOWN_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=SERVER_SHUTDOWN_TIMEOUT_SECONDS)


def run_verification_mode(server_executable: Path, client_executable: Path, mode: str) -> None:
    port = choose_port()
    shutil.rmtree(RUNTIME_DIR, ignore_errors=True)

    server_process = subprocess.Popen(
        server_command(server_executable, port, mode),
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        text=True,
    )

    try:
        wait_for_server(server_process)

        client_process = subprocess.Popen(
            [str(client_executable), HOST, str(port), "AC-101"],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.PIPE,
            text=True,
        )

        client_output, _ = client_process.communicate("1\nD\nQ\n", timeout=CLIENT_TIMEOUT_SECONDS)

        if client_process.returncode != 0:
            raise RuntimeError(
                f"Client exited with code {client_process.returncode}.\n"
                f"Client output:\n{client_output}"
            )

        if "Handshake verified" not in client_output:
            raise AssertionError(
                "Client output does not show a successful handshake.\n"
                f"Client output:\n{client_output}"
            )

        verify_logs()
    finally:
        terminate_process(server_process)


def main() -> int:
    server_executable = find_executable("ground_server")
    client_executable = find_executable("aircraft_client")
    modes = ["gui", "headless"] if PREFER_GUI else ["headless"]
    failures: list[str] = []

    for mode in modes:
        try:
            run_verification_mode(server_executable, client_executable, mode)
            print(f"Verification passed using {mode} mode.")
            print(f"Server executable: {server_executable}")
            print(f"Client executable: {client_executable}")
            return 0
        except Exception as exc:
            failures.append(f"{mode}: {exc}")

    raise RuntimeError("All verification modes failed.\n" + "\n\n".join(failures))


if __name__ == "__main__":
    sys.exit(main())
