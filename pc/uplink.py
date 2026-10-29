"""Forward the PLAYCE LoRa receiver's serial (and optionally Wi-Fi) JSON lines to the PLAYCE backend.

Run it on whatever machine the receiver is plugged into (Windows PC, laptop, Raspberry Pi) as long
as it can reach the backend; https://playce-admin.mooo.com/live and /qr then show that data.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import queue
import re
import socket
import sys
import threading
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

import serial
from serial.tools import list_ports

HERE = Path(__file__).resolve().parent
ENV_PATH = HERE.parent / ".env"
DEFAULT_BACKEND = "https://playce-admin.mooo.com"
DEVICE_RE = re.compile(r"^playce-[0-9a-f]{6}$")
EPC_RE = re.compile(r"^(?:[0-9A-F]{2}){1,32}$")
LOOPBACK_HOSTS = {"localhost", "127.0.0.1", "::1"}
BATCH_WINDOW_SECONDS = 0.25
BATCH_MAX_MESSAGES = 100
BATCH_MAX_BYTES = 40_000  # backend JSON body limit is 64 kB
HTTP_TIMEOUT_SECONDS = 5
# The backend timestamps lines on arrival, so lines held up longer than this would distort dwell times.
STALE_SECONDS = 3
STATUS_INTERVAL_SECONDS = 30
REGISTRATION_ADDRESS = ("127.0.0.1", 8767)
REGISTRATION_EVENTS = {"seen", "lost", "delayed"}


class RefuseRedirect(HTTPRedirectHandler):
    """Never follow a redirect: urllib would replay the token header to the new host."""

    def redirect_request(self, *_args, **_kwargs):
        return None


def load_local_env(path: Path = ENV_PATH):
    if not path.exists():
        return
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip().strip("\"'")
        if key and key not in os.environ:
            os.environ[key] = value


def check_backend_url(url: str) -> str:
    """The token travels in a header, so plain http is allowed only to this machine."""
    parts = urlsplit(url)
    if parts.scheme == "https" or (parts.scheme == "http" and parts.hostname in LOOPBACK_HOSTS):
        return url.rstrip("/")
    raise ValueError(f"backend URL must be https:// (http:// only for localhost): {url}")


def parse_line(raw) -> dict | None:
    try:
        text = raw.decode("utf-8") if isinstance(raw, bytes) else raw
        message = json.loads(text.strip())
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    return message if isinstance(message, dict) else None


def forward_registration(sock: socket.socket, message: dict, address=REGISTRATION_ADDRESS):
    """Same hand-off as collector.py: registration_reader.py listens for direct UHF reads on UDP 8767.

    Only lines the backend would accept are passed on, so every challenge it creates can show on /qr.
    """
    device, epc = message.get("device"), message.get("epc")
    if (message.get("v") != 1 or message.get("tag_source") != "direct_uhf"
            or message.get("event") not in REGISTRATION_EVENTS
            or not isinstance(device, str) or not DEVICE_RE.fullmatch(device)
            or device != message.get("gateway")
            or not isinstance(epc, str) or not EPC_RE.fullmatch(epc)):
        return
    try:
        sock.sendto(json.dumps(message, separators=(",", ":")).encode("utf-8"), address)
    except OSError:
        pass


def chunk_by_size(items: list, limit: int = BATCH_MAX_BYTES, message=lambda item: item) -> list[list]:
    chunks, current, size = [], [], 0
    for item in items:
        length = len(json.dumps(message(item), separators=(",", ":")).encode("utf-8")) + 1
        if current and size + length > limit:
            chunks.append(current)
            current, size = [], 0
        current.append(item)
        size += length
    if current:
        chunks.append(current)
    return chunks


def transport_runs(batch: list[tuple[float, str, dict]]) -> list[tuple[str, list[tuple[float, dict]]]]:
    """Consecutive lines per transport, in arrival order (the backend's dedup depends on order)."""
    runs: list[tuple[str, list[tuple[float, dict]]]] = []
    for received_at, transport, message in batch:
        if runs and runs[-1][0] == transport:
            runs[-1][1].append((received_at, message))
        else:
            runs.append((transport, [(received_at, message)]))
    return runs


class Uplink:
    def __init__(self, backend: str, token: str, log=print, opener=None, clock=time.monotonic):
        self.url = check_backend_url(backend) + "/internal/rfid/events"
        self.token = token
        self.log = log
        self.opener = opener or build_opener(RefuseRedirect()).open
        self.clock = clock
        self.queue: queue.Queue = queue.Queue(maxsize=5000)
        self.stats = {"sent": 0, "accepted": 0, "dropped": 0, "failed_batches": 0}
        self.last_problem = None

    def put(self, message: dict, transport: str, block: bool = False):
        try:
            self.queue.put((self.clock(), transport, message), block=block)
        except queue.Full:
            self.stats["dropped"] += 1

    def send_batch(self, batch: list[tuple[float, str, dict]]):
        for transport, items in transport_runs(batch):
            for chunk in chunk_by_size(items, message=lambda item: item[1]):
                self._deliver(transport, chunk)

    def _deliver(self, transport: str, chunk: list[tuple[float, dict]]):
        # Staleness is judged right before each POST; a failed POST is retried once while still fresh.
        for _attempt in range(2):
            fresh = [item for item in chunk if self.clock() - item[0] <= STALE_SECONDS]
            self.stats["dropped"] += len(chunk) - len(fresh)
            chunk = fresh
            if not chunk or self.post(transport, [message for _, message in chunk]):
                return
        self.stats["dropped"] += len(chunk)

    def post(self, transport: str, messages: list[dict]) -> bool:
        body = json.dumps({"transport": transport, "messages": messages}, separators=(",", ":")).encode("utf-8")
        request = Request(self.url, data=body, method="POST", headers={
            "Content-Type": "application/json",
            "Accept": "application/json",
            "X-Khuround-Internal-Token": self.token,
            "User-Agent": "playce-uplink/1",
        })
        try:
            with self.opener(request, timeout=HTTP_TIMEOUT_SECONDS) as response:
                result = json.loads(response.read().decode("utf-8"))
            accepted = result.get("accepted") if isinstance(result, dict) else None
        except HTTPError as error:
            hint = " (check KHUROUND_RFID_UPLINK_TOKEN)" if error.code in (401, 503) else ""
            return self._failed(f"backend HTTP {error.code}{hint}")
        except (URLError, TimeoutError, OSError, ValueError, http.client.HTTPException) as error:
            reason = getattr(error, "reason", None)
            return self._failed(f"backend unreachable: {reason if reason else repr(error)}")
        self.stats["sent"] += len(messages)
        self.stats["accepted"] += accepted if isinstance(accepted, int) else 0
        if self.last_problem is not None:
            self.log("Backend connection restored", flush=True)
            self.last_problem = None
        return True

    def _failed(self, problem: str) -> bool:
        self.stats["failed_batches"] += 1
        if problem != self.last_problem:
            self.log(problem, flush=True)
            self.last_problem = problem
        return False

    def drain(self, wait: float = 0.5) -> bool:
        """Collect one batch (up to the batch window) and send it; False if nothing arrived."""
        try:
            batch = [self.queue.get(timeout=wait)]
        except queue.Empty:
            return False
        deadline = self.clock() + BATCH_WINDOW_SECONDS
        while len(batch) < BATCH_MAX_MESSAGES:
            remaining = deadline - self.clock()
            if remaining <= 0:
                break
            try:
                batch.append(self.queue.get(timeout=remaining))
            except queue.Empty:
                break
        self.send_batch(batch)
        return True

    def run(self, stop: threading.Event):
        next_status = self.clock() + STATUS_INTERVAL_SECONDS
        while not stop.is_set():
            self._guarded_drain()
            if self.clock() >= next_status:
                self.log("Uplink: sent {sent}, accepted {accepted}, dropped {dropped}".format(**self.stats), flush=True)
                next_status = self.clock() + STATUS_INTERVAL_SECONDS
        while self._guarded_drain(wait=0):  # flush what is still queued at shutdown
            pass

    def _guarded_drain(self, wait: float = 0.5) -> bool:
        # The sender thread must outlive any surprise, or every later line would silently queue up.
        try:
            return self.drain(wait)
        except Exception as error:  # noqa: BLE001
            self.log(f"Uplink sender error: {error!r}", flush=True)
            return True


def serial_lines(port_name: str, stop: threading.Event, on_message, log=print):
    """Same port probing as collector.receiver_lines: a port counts once it sends a gateway line."""
    while not stop.is_set():
        ports = [port_name] if port_name != "auto" else [port.device for port in list_ports.comports()]
        for port in ports:
            if stop.is_set():
                break
            try:
                with serial.Serial(port, 115200, timeout=1) as connection:
                    log(f"Checking receiver on {port}", flush=True)
                    identified = False
                    deadline = time.monotonic() + 8
                    while not stop.is_set():
                        if port_name == "auto" and not identified and time.monotonic() > deadline:
                            break
                        raw = connection.readline()
                        if not raw:
                            continue
                        message = parse_line(raw)
                        if message is None:
                            continue
                        if not identified:
                            if message.get("event") != "gateway":
                                continue
                            identified = True
                            log(f"Receiver found: {message.get('device')} on {port}", flush=True)
                        on_message(message, "usb")
            except (serial.SerialException, OSError) as error:
                if port_name != "auto":
                    log(f"Waiting for {port}: {error}", flush=True)
        stop.wait(2)


def wifi_lines(port: int, stop: threading.Event, on_message, allow: set[str] | None = None, log=print):
    """Receiver lines over Wi-Fi UDP. Anyone on the LAN can send UDP, so pass `allow` (receiver IPs)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
            connection.bind(("0.0.0.0", port))
            connection.settimeout(1)
            log(f"Wi-Fi receiver listening on UDP {port}" + (f" from {', '.join(sorted(allow))}" if allow else ""),
                flush=True)
            while not stop.is_set():
                try:
                    raw, address = connection.recvfrom(2048)
                except socket.timeout:
                    continue
                if len(raw) > 1024 or (allow and address[0] not in allow):
                    continue
                message = parse_line(raw)
                if message is not None:
                    on_message(message, "wifi")
    except OSError as error:
        log(f"Wi-Fi receiver unavailable: {error}", flush=True)


def main():
    load_local_env()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="auto", help="Receiver serial port (COM5, /dev/ttyUSB0), auto, or off")
    parser.add_argument("--wifi-port", type=int, default=0,
                        help="Also accept Wi-Fi receiver lines on this UDP port (collector.py uses 8766); 0 = off")
    parser.add_argument("--wifi-allow", default="",
                        help="Comma-separated receiver IPs allowed to send Wi-Fi lines (recommended with --wifi-port)")
    parser.add_argument("--backend", default=os.environ.get("PLAYCE_BACKEND_URL", DEFAULT_BACKEND))
    parser.add_argument("--stdin", action="store_true", help="Read JSON lines from stdin instead (replay/testing)")
    parser.add_argument("--registration-port", type=int, default=REGISTRATION_ADDRESS[1],
                        help="Local UDP port of registration_reader.py, 0 = off")
    args = parser.parse_args()
    # Field machines should hold the narrow uplink token; the master internal token also works.
    token = (os.environ.get("KHUROUND_RFID_UPLINK_TOKEN") or os.environ.get("KHUROUND_INTERNAL_API_TOKEN") or "").strip()
    if not token:
        print(f"KHUROUND_RFID_UPLINK_TOKEN is not set (environment or {ENV_PATH})", file=sys.stderr, flush=True)
        raise SystemExit(2)
    try:
        uplink = Uplink(args.backend, token)
    except ValueError as error:
        print(error, file=sys.stderr, flush=True)
        raise SystemExit(2) from error
    allow = {address.strip() for address in args.wifi_allow.split(",") if address.strip()} or None
    registration_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    registration_address = ("127.0.0.1", args.registration_port)

    def on_message(message: dict, transport: str, block: bool = False):
        uplink.put(message, transport, block=block)
        if args.registration_port:
            forward_registration(registration_socket, message, registration_address)

    print(f"Uplink to {uplink.url}", flush=True)
    stop = threading.Event()
    sender = threading.Thread(target=uplink.run, args=(stop,), daemon=True)
    sender.start()
    workers = []
    if args.stdin:
        for line in sys.stdin:
            message = parse_line(line)
            if message is not None:
                on_message(message, "usb", block=True)
    else:
        if args.port != "off":
            workers.append(threading.Thread(target=serial_lines, args=(args.port, stop, on_message), daemon=True))
        if args.wifi_port:
            if not allow:
                print("Warning: --wifi-port without --wifi-allow accepts lines from any LAN host", flush=True)
            workers.append(threading.Thread(target=wifi_lines, args=(args.wifi_port, stop, on_message, allow),
                                            daemon=True))
        for worker in workers:
            worker.start()
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
    stop.set()
    sender.join(timeout=60)
    for worker in workers:
        worker.join(timeout=3)
    registration_socket.close()
    print("Uplink stopped: sent {sent}, accepted {accepted}, dropped {dropped}".format(**uplink.stats), flush=True)


if __name__ == "__main__":
    main()
