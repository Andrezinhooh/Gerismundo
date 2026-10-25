#!/usr/bin/env python3
"""Exercise the real signaling executable over localhost WebSocket.

Usage: python tests/signaling_integration.py path/to/native-call-signaling
Requires the Python test-only package `websockets`; it is not a runtime dependency.
"""

import asyncio
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import time

import websockets


TOKEN = "integration-test-token-32-bytes-long"


async def receive_type(connection, wanted, **fields):
    deadline = asyncio.get_running_loop().time() + 5
    while True:
        remaining = deadline - asyncio.get_running_loop().time()
        message = json.loads(await asyncio.wait_for(connection.recv(), remaining))
        if message.get("type") == wanted and all(message.get(k) == v for k, v in fields.items()):
            return message


async def send(connection, **message):
    await connection.send(json.dumps(message))


async def check_server(url):
    opened = []

    async def connect():
        connection = await websockets.connect(url, proxy=None, open_timeout=5)
        opened.append(connection)
        return connection

    async def join(name, room="integration", token=TOKEN):
        connection = await connect()
        await send(connection, type="join", room=room, name=name, token=token)
        return connection, await receive_type(connection, "welcome")

    try:
        # Authoritative participant limit: the fifth join must leave the first four alive.
        members = []
        for index in range(4):
            connection, welcome = await join(f"Pessoa {index + 1}")
            assert len(welcome["peers"]) == index, welcome
            assert len(welcome["id"]) == 32, welcome
            members.append((connection, welcome["id"]))
        fifth = await connect()
        await send(fifth, type="join", room="integration", name="Quinta", token=TOKEN)
        assert (await receive_type(fifth, "error"))["code"] == "room_full"
        for connection, _ in members:
            await send(connection, type="ping")
            await receive_type(connection, "pong")

        # Sender identity must come from the authenticated socket, never a JSON `from`.
        a, a_id = members[0]
        b, b_id = members[1]
        await send(a, type="candidate", to=b_id, candidate="candidate:test", mid="0", **{"from": "spoofed"})
        relayed = await receive_type(b, "candidate")
        assert relayed["from"] == a_id and relayed["to"] == b_id, relayed

        await send(a, type="description", to=b_id, descriptionType="offer", sdp="v=0\r\n")
        relayed = await receive_type(b, "description")
        assert relayed["descriptionType"] == "offer" and relayed["sdp"] == "v=0\r\n"
        await send(a, type="reset", to=b_id)
        assert (await receive_type(b, "reset"))["from"] == a_id

        # Independent shared-media metadata reaches other peers.
        await send(a, type="share", source="Jogo — janela", active=True)
        sharing = await receive_type(b, "share")
        assert sharing["from"] == a_id and sharing["active"] and sharing["source"] == "Jogo — janela"

        # Leaving frees a slot without ending the call for other participants.
        departing, departing_id = members.pop()
        await send(departing, type="leave")
        await receive_type(a, "peer-left", id=departing_id)
        replacement, replacement_welcome = await join("Substituta")
        assert len(replacement_welcome["peers"]) == 3, replacement_welcome
        first_state = next(peer for peer in replacement_welcome["peers"] if peer["id"] == a_id)
        assert first_state["sharing"] and first_state["source"] == "Jogo — janela"
        await send(replacement, type="ping")
        await receive_type(replacement, "pong")

        # Shared server credentials do not permit relaying into a different room.
        outsider, _ = await join("Outra sala", room="other-room")
        await send(outsider, type="candidate", to=b_id, candidate="forbidden", mid="0")
        assert (await receive_type(outsider, "error"))["code"] == "bad_request"
        await send(b, type="ping")
        while True:
            message = json.loads(await asyncio.wait_for(b.recv(), 5))
            assert message.get("candidate") != "forbidden", message
            if message["type"] == "pong":
                break

        unauthorized = await connect()
        await send(unauthorized, type="join", room="integration", name="Intrusa", token="wrong-token")
        assert (await receive_type(unauthorized, "error"))["code"] == "unauthorized"

        malformed = await connect()
        await malformed.send("not JSON")
        assert (await receive_type(malformed, "error"))["code"] == "bad_request"

        nested = await connect()
        await nested.send('{"unused":' + '[' * 100 + '0' + ']' * 100 + '}')
        assert (await receive_type(nested, "error"))["code"] == "bad_request"

        # Rejected/malformed clients must not disturb established participants.
        for connection, _ in members:
            await send(connection, type="ping")
            await receive_type(connection, "pong")
        print("PASS: four participants, fifth rejected, leave/rejoin, SDP/ICE/reset relay,")
        print("      sender binding, share metadata, room isolation, auth, malformed JSON, bounded depth.")
    finally:
        await asyncio.gather(*(connection.close() for connection in opened), return_exceptions=True)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    with socket.socket() as port_probe:
        port_probe.bind(("127.0.0.1", 0))
        port = port_probe.getsockname()[1]
    environment = os.environ.copy()
    environment["NATIVE_CALL_TOKEN"] = TOKEN
    process = subprocess.Popen(
        [executable, "--bind", "127.0.0.1", "--port", str(port)],
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
                    raise TimeoutError("O servidor não iniciou em quinze segundos.")
                time.sleep(.05)
        asyncio.run(check_server(f"ws://127.0.0.1:{port}"))
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
    main()
