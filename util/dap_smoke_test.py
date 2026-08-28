#!/usr/bin/env python
# End-to-end smoke test for the Wren DAP debugger.
#
# Launches example/debugger/wren_debug_example on a local port, connects as a
# Debug Adapter Protocol client, and drives a full debugging session:
# breakpoints, entry stop, stack traces, stepping, and variable inspection.
#
# Usage: python3 util/dap_smoke_test.py

import json
import os
import queue
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_BINARY = os.path.join(ROOT, "projects", "cmake", "build", "wren_debug_example")
SCRIPT = os.path.join(ROOT, "example", "debugger", "smoke.wren")

BREAKPOINT_LINE = 5


def fail(message):
    print("FAIL: " + message)
    sys.exit(1)


class DapClient:
    """A minimal DAP client: framed reads on a background thread."""

    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=20)
        self.messages = queue.Queue()
        self.pending = []
        self.sequence = 1
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _read_loop(self):
        buffer = b""
        while True:
            try:
                chunk = self.socket.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buffer += chunk
            while True:
                header_end = buffer.find(b"\r\n\r\n")
                if header_end < 0:
                    break
                header = buffer[:header_end].decode("utf-8", "replace")
                length = None
                for line in header.split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        length = int(line.split(":", 1)[1].strip())
                if length is None or len(buffer) < header_end + 4 + length:
                    break
                payload = buffer[header_end + 4:header_end + 4 + length]
                buffer = buffer[header_end + 4 + length:]
                self.messages.put(json.loads(payload.decode("utf-8")))

    def request(self, command, arguments=None):
        request = {
            "seq": self.sequence,
            "type": "request",
            "command": command,
        }
        self.sequence += 1
        if arguments is not None:
            request["arguments"] = arguments
        self._send(request)
        return request["seq"]

    def _send(self, message):
        payload = json.dumps(message).encode("utf-8")
        frame = ("Content-Length: %d\r\n\r\n" % len(payload)).encode("ascii")
        self.socket.sendall(frame + payload)

    def _matches(self, message, kind, event, command):
        if message.get("type") != kind:
            return False
        if event is not None and message.get("event") != event:
            return False
        if command is not None and message.get("command") != command:
            return False
        return True

    def wait_for(self, kind, event=None, command=None, timeout=10):
        """Returns the next matching message, buffering non-matching ones.

        Responses and events can arrive in either order, so skipped messages
        are kept for later waits instead of being dropped.
        """
        for index, message in enumerate(self.pending):
            if self._matches(message, kind, event, command):
                return self.pending.pop(index)

        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                message = self.messages.get(timeout=deadline - time.time())
            except queue.Empty:
                break
            if self._matches(message, kind, event, command):
                return message
            self.pending.append(message)
        descriptor = event if event is not None else (command or kind)
        fail("timed out waiting for " + descriptor)


def find_free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def start_host(port):
    process = subprocess.Popen(
        [HOST_BINARY, "--port", str(port), "--wait-ms", "20000", SCRIPT],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE)
    # Give the server a moment to bind.
    time.sleep(0.5)
    if process.poll() is not None:
        fail("debug host exited immediately with code %d" % process.returncode)
    return process


def main():
    if not os.path.exists(HOST_BINARY):
        fail("missing %s; build it first (make wren_debug_example)" % HOST_BINARY)

    port = find_free_port()
    process = start_host(port)
    client = DapClient(port)

    # -- Handshake -----------------------------------------------------------
    client.request("initialize", {"adapterID": "wren"})
    response = client.wait_for("response", command="initialize")
    if not response.get("body", {}).get("supportsConfigurationDoneRequest"):
        fail("adapter did not advertise configurationDone support")
    client.wait_for("event", event="initialized")

    # -- Breakpoints ---------------------------------------------------------
    client.request("setBreakpoints", {
        "source": {"name": "smoke.wren", "path": SCRIPT},
        "lines": [BREAKPOINT_LINE],
        "breakpoints": [{"line": BREAKPOINT_LINE}],
    })
    response = client.wait_for("response", command="setBreakpoints")
    if not response["body"]["breakpoints"][0]["verified"]:
        fail("breakpoint was not accepted")

    client.request("configurationDone")
    client.wait_for("response", command="configurationDone")

    # -- Stop on entry -------------------------------------------------------
    stopped = client.wait_for("event", event="stopped")
    if stopped["body"]["reason"] != "entry":
        fail("expected entry stop, got " + stopped["body"]["reason"])

    client.request("stackTrace", {"threadId": 1})
    response = client.wait_for("response", command="stackTrace")
    frames = response["body"]["stackFrames"]
    if len(frames) != 1:
        fail("expected 1 frame at entry, got %d" % len(frames))
    if frames[0]["source"]["path"] != SCRIPT:
        fail("frame source path mismatch: " + frames[0]["source"]["path"])

    # -- Step over -----------------------------------------------------------
    client.request("next", {"threadId": 1})
    client.wait_for("response", command="next")
    stopped = client.wait_for("event", event="stopped")
    if stopped["body"]["reason"] != "step":
        fail("expected step stop, got " + stopped["body"]["reason"])

    # -- Run to the breakpoint ----------------------------------------------
    client.request("continue", {"threadId": 1})
    client.wait_for("response", command="continue")
    stopped = client.wait_for("event", event="stopped")
    if stopped["body"]["reason"] != "breakpoint":
        fail("expected breakpoint stop, got " + stopped["body"]["reason"])

    client.request("stackTrace", {"threadId": 1})
    response = client.wait_for("response", command="stackTrace")
    line = response["body"]["stackFrames"][0]["line"]
    if line != BREAKPOINT_LINE:
        fail("stopped on line %d, expected %d" % (line, BREAKPOINT_LINE))

    # -- Inspect scopes and variables ---------------------------------------
    client.request("scopes", {"frameId": 0})
    response = client.wait_for("response", command="scopes")
    scopes = {scope["name"]: scope["variablesReference"]
              for scope in response["body"]["scopes"]}
    if "Locals" not in scopes:
        fail("no Locals scope reported")
    if not any(name.startswith("Module:") for name in scopes):
        fail("no module variables scope reported")

    module_scope = next(name for name in scopes if name.startswith("Module:"))

    def module_variable(name):
        # Scopes (and their variablesReferences) are only valid for the
        # current stop, so re-fetch them each time.
        client.request("scopes", {"frameId": 0})
        response = client.wait_for("response", command="scopes")
        current_scopes = {scope["name"]: scope["variablesReference"]
                          for scope in response["body"]["scopes"]}
        module_ref = next(current_scopes[scope_name]
                          for scope_name in current_scopes
                          if scope_name.startswith("Module:"))
        client.request("variables", {"variablesReference": module_ref})
        response = client.wait_for("response", command="variables")
        for variable in response["body"]["variables"]:
            if variable["name"] == name:
                return variable
        return None

    total = module_variable("total")
    if total is None or total["value"] != "0":
        fail("first breakpoint hit: total should be 0, got %r" %
             (total and total["value"]))

    # -- Second breakpoint hit: values must be re-read ----------------------
    client.request("continue", {"threadId": 1})
    client.wait_for("response", command="continue")
    stopped = client.wait_for("event", event="stopped")
    if stopped["body"]["reason"] != "breakpoint":
        fail("expected second breakpoint stop, got " + stopped["body"]["reason"])

    total = module_variable("total")
    if total is None or total["value"] != "1":
        fail("second breakpoint hit: total should be 1, got %r" %
             (total and total["value"]))

    # -- Third loop iteration, then run to completion -----------------------
    client.request("continue", {"threadId": 1})
    client.wait_for("response", command="continue")
    stopped = client.wait_for("event", event="stopped")
    if stopped["body"]["reason"] != "breakpoint":
        fail("expected third breakpoint stop, got " + stopped["body"]["reason"])

    client.request("continue", {"threadId": 1})
    client.wait_for("response", command="continue")
    client.wait_for("event", event="exited")
    client.wait_for("event", event="terminated")

    output = ""
    leftover = client.pending + list(client.messages.queue)
    client.pending.clear()
    for message in leftover:
        if message.get("type") == "event" and message.get("event") == "output":
            output += message["body"].get("output", "")
    while True:
        try:
            message = client.messages.get(timeout=2)
        except queue.Empty:
            break
        if message.get("type") == "event" and message.get("event") == "output":
            output += message["body"].get("output", "")
    if "total is 6" not in output:
        fail("expected script output event 'total is 6', got %r" % output)

    process.wait(timeout=10)
    if process.returncode != 0:
        fail("debug host exited with %d" % process.returncode)

    # -- Second session: stopOnEntry=false via the attach request -----------
    # The first stop should be the breakpoint, not an entry stop.
    port = find_free_port()
    process = start_host(port)
    client = DapClient(port)

    client.request("initialize", {"adapterID": "wren"})
    client.wait_for("response", command="initialize")
    client.wait_for("event", event="initialized")
    client.request("attach", {"stopOnEntry": False})
    client.wait_for("response", command="attach")
    client.request("setBreakpoints", {
        "source": {"name": "smoke.wren", "path": SCRIPT},
        "lines": [BREAKPOINT_LINE],
        "breakpoints": [{"line": BREAKPOINT_LINE}],
    })
    client.wait_for("response", command="setBreakpoints")
    client.request("configurationDone")
    client.wait_for("response", command="configurationDone")

    stopped = client.wait_for("event", event="stopped")
    if stopped["body"]["reason"] != "breakpoint":
        fail("with stopOnEntry=false, expected breakpoint stop, got " +
             stopped["body"]["reason"])

    process.kill()
    print("DAP smoke test passed.")


if __name__ == "__main__":
    main()
