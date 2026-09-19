#!/usr/bin/env python3
"""Detach any stale gdbstub session so a VM left stopped by a debug client
resumes (QEMU continues the guest on 'D').  usage: gdb_detach.py [port]"""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 1234
s = socket.create_connection(("127.0.0.1", port), timeout=3)
s.sendall(b"+$D#44")
try:
    print(s.recv(64))
except socket.timeout:
    print("no reply")
s.close()
