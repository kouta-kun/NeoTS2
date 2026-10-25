#!/usr/bin/env python3
"""Reads the emulated console's memory through PCSX2's PINE interface (EnablePINE = true in PCSX2.ini; the socket is $XDG_RUNTIME_DIR/pcsx2.sock).
Usage: pine_read.py <hex address> [count of 32-bit words]    prints the words (hex)."""
import os, socket, struct, sys

def sock_path():
    base = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
    for name in ("pcsx2.sock", "pcsx2.sock.28011"):
        path = os.path.join(base, name)
        if os.path.exists(path):
            return path
    return os.path.join("/tmp", "pcsx2.sock")

def read32s(addresses):
    """One batched request: a read of 32 bits at each address."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path())
    body = b"".join(struct.pack("<BI", 2, a) for a in addresses)  # MsgRead32 = 2
    request = struct.pack("<I", 4 + len(body)) + body
    s.sendall(request)
    head = b""
    while len(head) < 5:
        head += s.recv(5 - len(head))
    size, result = struct.unpack("<IB", head)
    if result != 0:
        raise RuntimeError("PINE error %d" % result)
    data = b""
    while len(data) < size - 5:
        data += s.recv(size - 5 - len(data))
    s.close()
    return struct.unpack("<%dI" % len(addresses), data) if len(data) == 4 * len(addresses) else struct.unpack("<%dI" % (len(data) // 4), data)

if __name__ == "__main__":
    base = int(sys.argv[1], 16)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    for i, w in enumerate(read32s([base + 4 * i for i in range(n)])):
        print("%08x: %08x" % (base + 4 * i, w))
