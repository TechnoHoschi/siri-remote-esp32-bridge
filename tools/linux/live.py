#!/usr/bin/env python3
"""Live-Ansicht im Browser: zeigt Tastendrücke der Siri Remote samt Timing.

Startet den Monitor (tools/linux/monitor.py) und einen kleinen HTTP-Server, der die Ereignisse per
Server-Sent Events an tools/linux/live.html schickt. Die Log-Zeilen laufen zusätzlich nach stdout.

Aufruf: .venv/bin/python -u tools/linux/live.py <identity-adresse> [--port 8765]
"""
import asyncio
import json
import pathlib
from collections import deque

import monitor

HTML = pathlib.Path(__file__).with_name("live.html")
clients: set[asyncio.Queue] = set()
backlog: deque = deque(maxlen=300)


def publish(ev: dict) -> None:
    print(monitor.format_event(ev), flush=True)
    # Touch- und Mikrofon-Reports kommen mit ~65/s: live durchreichen, aber nicht aufheben.
    if ev["type"] != "report" or ev["id"] == monitor.BUTTON_REPORT:
        backlog.append(ev)
    for queue in clients:
        queue.put_nowait(ev)


async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    try:
        request = (await reader.readline()).split()
        path = request[1].decode() if len(request) > 1 else "/"
        while (await reader.readline()) not in (b"\r\n", b"\n", b""):
            pass
        if path == "/events":
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                         b"Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n")
            queue: asyncio.Queue = asyncio.Queue()
            for ev in backlog:
                queue.put_nowait(ev)
            clients.add(queue)
            try:
                while True:
                    try:
                        ev = await asyncio.wait_for(queue.get(), 15)
                        writer.write(f"data: {json.dumps(ev)}\n\n".encode())
                    except asyncio.TimeoutError:
                        writer.write(b": ping\n\n")
                    await writer.drain()
            finally:
                clients.discard(queue)
        elif path == "/":
            body = HTML.read_bytes()
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                         b"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(body))
            writer.write(body)
            await writer.drain()
        else:
            writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            await writer.drain()
    except (ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        writer.close()


async def main() -> None:
    ap = monitor.parse_args(__doc__)
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()
    server = await asyncio.start_server(handle, "0.0.0.0", args.port)
    async with server:
        await monitor.run(args.address, args.enable, publish)


if __name__ == "__main__":
    asyncio.run(main())
