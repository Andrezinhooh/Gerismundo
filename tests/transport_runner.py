#!/usr/bin/env python3
"""Start the real signaling server and run the C++ native WebRTC mesh test.

Usage: python tests/transport_runner.py path/to/signaling path/to/transport-test
No Python package beyond the standard library is needed for this test runner.
"""
import os
import pathlib
import signal
import socket
import subprocess
import sys
import time


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    server, test = (str(pathlib.Path(path).resolve()) for path in sys.argv[1:])
    with socket.socket() as port_probe:
        port_probe.bind(("127.0.0.1", 0))
        port = port_probe.getsockname()[1]
    environment = os.environ.copy()
    environment["NATIVE_CALL_TOKEN"] = "local-transport-test-fixture-token"
    environment["NATIVE_CALL_SIGNALING_URL"] = f"ws://127.0.0.1:{port}"
    process = subprocess.Popen(
        [server, "--bind", "127.0.0.1", "--port", str(port)],
        env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    try:
        deadline = time.monotonic() + 15
        while True:
            if process.poll() is not None:
                raise RuntimeError(process.stderr.read().decode(errors="replace"))
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.2):
                    break
            except OSError:
                if time.monotonic() >= deadline:
                    raise TimeoutError("Signaling server did not start in fifteen seconds.")
                time.sleep(.05)
        result = subprocess.run([test], env=environment, timeout=50, check=False)
        return result.returncode
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        process.stderr.close()


if __name__ == "__main__":
    raise SystemExit(main())
