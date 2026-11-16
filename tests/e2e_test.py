#!/usr/bin/env python3
"""End-to-end tests: start a server binary, talk raw RESP over TCP, and check
the exact reply bytes.

    python3 tests/e2e_test.py <server-binary> [port]

The binary is started as `<server-binary> <port>`. Standard library only.
"""

import os
import socket
import subprocess
import sys
import time
import unittest

BINARY = None
PORT = 7001


def resp(*args: str) -> bytes:
    """Encode a command as a RESP array of bulk strings."""
    out = f"*{len(args)}\r\n"
    for a in args:
        out += f"${len(a.encode())}\r\n{a}\r\n"
    return out.encode()


class Conn:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=2)
        self.buf = b""

    def send(self, data: bytes):
        self.sock.sendall(data)

    def read_exact(self, n_bytes: int, timeout: float = 2.0) -> bytes:
        self.sock.settimeout(timeout)
        while len(self.buf) < n_bytes:
            chunk = self.sock.recv(65536)
            if not chunk:
                break
            self.buf += chunk
        out, self.buf = self.buf[:n_bytes], self.buf[n_bytes:]
        return out

    def call(self, *args: str, expect: bytes) -> bytes:
        self.send(resp(*args))
        return self.read_exact(len(expect))

    def nothing_pending(self, wait: float = 0.15) -> bool:
        self.sock.settimeout(wait)
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return self.buf == b""
        self.buf += data
        return False

    def close(self):
        self.sock.close()


class E2E(unittest.TestCase):
    server: subprocess.Popen

    @classmethod
    def setUpClass(cls):
        cls.server = subprocess.Popen(
            [BINARY, str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        cls.server.kill()
        raise RuntimeError(f"server {BINARY} did not start on port {PORT}")

    @classmethod
    def tearDownClass(cls):
        cls.server.kill()
        cls.server.wait()

    def setUp(self):
        self.c = Conn()

    def tearDown(self):
        self.c.close()

    def check(self, *args, expect: bytes, conn=None):
        c = conn or self.c
        self.assertEqual(c.call(*args, expect=expect), expect, f"command {args}")

    # --- basics -----------------------------------------------------------

    def test_ping_echo(self):
        self.check("PING", expect=b"+PONG\r\n")
        self.check("ECHO", "hello world", expect=b"$11\r\nhello world\r\n")
        self.check("ECHO", expect=b"-ERR wrong number of arguments\r\n")

    def test_case_insensitive_and_unknown(self):
        self.check("ping", expect=b"+PONG\r\n")
        self.check("NOPE", expect=b"-ERR unknown command\r\n")

    def test_partial_and_pipelined_input(self):
        raw = resp("SET", "pk", "v") + resp("GET", "pk") + resp("PING")
        for i in range(0, len(raw), 3):  # dribble it in 3-byte pieces
            self.c.send(raw[i : i + 3])
            time.sleep(0.002)
        expect = b"+OK\r\n$1\r\nv\r\n+PONG\r\n"
        self.assertEqual(self.c.read_exact(len(expect)), expect)

    # --- strings ----------------------------------------------------------

    def test_set_get(self):
        self.check("SET", "s1", "abc", expect=b"+OK\r\n")
        self.check("GET", "s1", expect=b"$3\r\nabc\r\n")
        self.check("GET", "missing", expect=b"$-1\r\n")
        self.check("SET", "s1", expect=b"-ERR wrong number of arguments\r\n")

    def test_set_px_expiry(self):
        self.check("SET", "tmp", "x", "PX", "100", expect=b"+OK\r\n")
        self.check("GET", "tmp", expect=b"$1\r\nx\r\n")
        time.sleep(0.2)
        self.check("GET", "tmp", expect=b"$-1\r\n")
        self.check("TYPE", "tmp", expect=b"+none\r\n")

    def test_set_clears_old_ttl(self):
        self.check("SET", "t2", "x", "PX", "100", expect=b"+OK\r\n")
        self.check("SET", "t2", "y", expect=b"+OK\r\n")
        time.sleep(0.2)
        self.check("GET", "t2", expect=b"$1\r\ny\r\n")

    # --- lists ------------------------------------------------------------

    def test_push_range_len(self):
        self.check("RPUSH", "l1", "a", "b", "c", expect=b":3\r\n")
        self.check("LPUSH", "l1", "x", "y", expect=b":5\r\n")  # y x a b c
        self.check("LLEN", "l1", expect=b":5\r\n")
        self.check("LLEN", "nolist", expect=b":0\r\n")
        self.check("LRANGE", "l1", "0", "1", expect=b"*2\r\n$1\r\ny\r\n$1\r\nx\r\n")
        self.check("LRANGE", "l1", "-2", "-1", expect=b"*2\r\n$1\r\nb\r\n$1\r\nc\r\n")
        self.check("LRANGE", "l1", "3", "100", expect=b"*2\r\n$1\r\nb\r\n$1\r\nc\r\n")
        self.check("LRANGE", "l1", "4", "1", expect=b"*0\r\n")
        self.check("LRANGE", "nolist", "0", "-1", expect=b"*0\r\n")

    def test_lpop(self):
        self.check("RPUSH", "l2", "a", "b", "c", "d", expect=b":4\r\n")
        self.check("LPOP", "l2", expect=b"$1\r\na\r\n")
        self.check("LPOP", "l2", "2", expect=b"*2\r\n$1\r\nb\r\n$1\r\nc\r\n")
        self.check("LPOP", "l2", "10", expect=b"*1\r\n$1\r\nd\r\n")
        self.check("LPOP", "l2", expect=b"$-1\r\n")
        self.check("LPOP", "nolist", expect=b"$-1\r\n")

    def test_type(self):
        self.check("SET", "ts", "v", expect=b"+OK\r\n")
        self.check("RPUSH", "tl", "v", expect=b":1\r\n")
        self.check("TYPE", "ts", expect=b"+string\r\n")
        self.check("TYPE", "tl", expect=b"+list\r\n")
        self.check("TYPE", "nothing", expect=b"+none\r\n")

    # --- BLPOP ------------------------------------------------------------

    def test_blpop_immediate(self):
        self.check("RPUSH", "b1", "v", expect=b":1\r\n")
        self.check("BLPOP", "b1", "0", expect=b"*2\r\n$2\r\nb1\r\n$1\r\nv\r\n")

    def test_blpop_woken_by_push(self):
        other = Conn()
        try:
            self.c.send(resp("BLPOP", "b2", "0"))
            self.assertTrue(self.c.nothing_pending(), "BLPOP should block")
            self.check("RPUSH", "b2", "hi", expect=b":1\r\n", conn=other)
            expect = b"*2\r\n$2\r\nb2\r\n$2\r\nhi\r\n"
            self.assertEqual(self.c.read_exact(len(expect)), expect)
        finally:
            other.close()

    def test_blpop_fifo_waiters(self):
        w1, w2, pusher = Conn(), Conn(), Conn()
        try:
            w1.send(resp("BLPOP", "b3", "0"))
            time.sleep(0.05)
            w2.send(resp("BLPOP", "b3", "0"))
            time.sleep(0.05)
            self.check("RPUSH", "b3", "first", "second", expect=b":2\r\n", conn=pusher)
            e1 = b"*2\r\n$2\r\nb3\r\n$5\r\nfirst\r\n"
            e2 = b"*2\r\n$2\r\nb3\r\n$6\r\nsecond\r\n"
            self.assertEqual(w1.read_exact(len(e1)), e1)
            self.assertEqual(w2.read_exact(len(e2)), e2)
        finally:
            for c in (w1, w2, pusher):
                c.close()

    def test_blpop_timeout(self):
        start = time.time()
        self.check("BLPOP", "b4", "0.2", expect=b"*-1\r\n")
        self.assertGreaterEqual(time.time() - start, 0.15)

    def test_blpop_then_pipelined_command(self):
        # Commands sent while blocked run after the BLPOP is served.
        other = Conn()
        try:
            self.c.send(resp("BLPOP", "b5", "0") + resp("PING"))
            self.assertTrue(self.c.nothing_pending())
            self.check("LPUSH", "b5", "z", expect=b":1\r\n", conn=other)
            expect = b"*2\r\n$2\r\nb5\r\n$1\r\nz\r\n+PONG\r\n"
            self.assertEqual(self.c.read_exact(len(expect)), expect)
        finally:
            other.close()

    def test_blocked_client_disconnect(self):
        w = Conn()
        w.send(resp("BLPOP", "b6", "0"))
        time.sleep(0.05)
        w.close()
        time.sleep(0.05)
        self.check("RPUSH", "b6", "v", expect=b":1\r\n")
        self.check("LLEN", "b6", expect=b":1\r\n")  # nobody consumed it

    # --- streams ----------------------------------------------------------

    def test_xadd_explicit_ids(self):
        self.check("XADD", "st1", "1-1", "f", "v", expect=b"$3\r\n1-1\r\n")
        self.check("XADD", "st1", "1-2", "f", "v", expect=b"$3\r\n1-2\r\n")
        self.check(
            "XADD", "st1", "1-2", "f", "v",
            expect=b"-ERR The ID specified in XADD is equal or smaller than the target stream top item\r\n",
        )
        self.check(
            "XADD", "st1", "0-0", "f", "v",
            expect=b"-ERR The ID specified in XADD must be greater than 0-0\r\n",
        )
        self.check("TYPE", "st1", expect=b"+stream\r\n")

    def test_xadd_partial_auto_seq(self):
        self.check("XADD", "st2", "5-*", "f", "v", expect=b"$3\r\n5-1\r\n")
        self.check("XADD", "st2", "5-*", "f", "v", expect=b"$3\r\n5-2\r\n")
        self.check("XADD", "st2", "6-*", "f", "v", expect=b"$3\r\n6-0\r\n")

    def test_xadd_full_auto(self):
        self.c.send(resp("XADD", "st3", "*", "f", "v"))
        reply = b""
        while reply.count(b"\r\n") < 2:
            reply += self.c.read_exact(1)
        entry_id = reply.split(b"\r\n")[1].decode()
        ts, seq = entry_id.split("-")
        self.assertAlmostEqual(int(ts) / 1000, time.time(), delta=5)
        self.assertEqual(seq, "0")

    def test_xadd_errors(self):
        self.check("XADD", "st4", "1-1", "f", expect=b"-ERR wrong number of arguments for XADD\r\n")
        self.check("XADD", "st4", "bad", "f", "v", expect=b"-ERR invalid entry ID format\r\n")

    def test_xrange(self):
        for i in (1, 2, 3):
            self.check("XADD", "st5", f"0-{i}", "k", f"v{i}", expect=f"$3\r\n0-{i}\r\n".encode())
        e = lambda i: f"*2\r\n$3\r\n0-{i}\r\n*2\r\n$1\r\nk\r\n$2\r\nv{i}\r\n".encode()
        self.check("XRANGE", "st5", "0-2", "0-3", expect=b"*2\r\n" + e(2) + e(3))
        self.check("XRANGE", "st5", "-", "0-1", expect=b"*1\r\n" + e(1))
        self.check("XRANGE", "st5", "0-2", "+", expect=b"*2\r\n" + e(2) + e(3))
        self.check("XRANGE", "st5", "0", "0", expect=b"*0\r\n")
        self.check("XRANGE", "nostream", "-", "+", expect=b"*0\r\n")

    def test_server_survives_everything(self):
        self.check("PING", expect=b"+PONG\r\n")


if __name__ == "__main__":
    BINARY = os.path.abspath(sys.argv[1])
    if len(sys.argv) > 2:
        PORT = int(sys.argv[2])
    unittest.main(argv=[sys.argv[0], "-v"], verbosity=1)
