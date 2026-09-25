import argparse
import json
import socket
import sys


def Main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=19840, type=int)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    if not arguments.command:
        parser.error("a monitor command is required")
    command = " ".join(arguments.command)
    connection = socket.create_connection((arguments.host, arguments.port), timeout=10)
    connection.sendall((command + "\n").encode("utf-8"))
    response = bytearray()
    while not response.endswith(b"\n"):
        block = connection.recv(4096)
        if not block:
            break
        response.extend(block)
    connection.close()
    decoded = response.decode("utf-8").strip()
    print(decoded)
    result = json.loads(decoded)
    if not result.get("ok"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(Main())
