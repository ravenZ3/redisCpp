#!/usr/bin/env python3
"""Open N idle TCP connections to a server and hold them until killed.

    python3 bench/idle_clients.py <port> <count>
"""

import signal
import socket
import sys

port, count = int(sys.argv[1]), int(sys.argv[2])
conns = [socket.create_connection(("127.0.0.1", port)) for _ in range(count)]
print(f"holding {len(conns)} idle connections", flush=True)
signal.sigwait({signal.SIGTERM, signal.SIGINT})
