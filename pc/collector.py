"""Receive PLAYCE LoRa gateway lines over Wi-Fi or USB and serve the dashboard."""

from __future__ import annotations

import argparse
from collections import deque
import csv
import io
import json
import os
import re
import sqlite3
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial
from serial.tools import list_ports

DEVICE_RE = re.compile(r"^playce-[0-9a-f]{6}$")
EPC_RE = re.compile(r"^(?:[0-9A-F]{2}){1,32}$")
EVENTS = {"gateway", "heartbeat", "seen", "delayed", "lost"}
TAG_DELAY_SECONDS = 5
TAG_DELAY_DISPLAY_SECONDS = 2
TAG_DISPLAY_SECONDS = TAG_DELAY_SECONDS + TAG_DELAY_DISPLAY_SECONDS
TAG_EXPIRE_SECONDS = 30
HERE = Path(__file__).resolve().parent
DATA_DIR = Path(os.environ.get("LOCALAPPDATA", HERE)) / "PLAYCE-RFID"
LOG_PATH = HERE.parent / "log.txt"
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/dashboard.css": ("dashboard.css", "text/css; charset=utf-8"),
    "/dashboard.js": ("dashboard.js", "text/javascript; charset=utf-8"),
    "/fonts/Freesentation-9Black.woff2": ("fonts/Freesentation-9Black.woff2", "font/woff2"),
    "/fonts/Paperlogy-5Medium.woff2": ("fonts/Paperlogy-5Medium.woff2", "font/woff2"),
    "/fonts/RIDIBatang.woff": ("fonts/RIDIBatang.woff", "font/woff"),
}


class MonitorLog:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.stream = path.open("a", encoding="utf-8", buffering=1)
        self.lock = threading.Lock()

    def write(self, source: str, data: dict):
        entry = {"time_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                 "source": source, **data}
        with self.lock:
            self.stream.write(json.dumps(entry, ensure_ascii=False, separators=(",", ":")) + "\n")

    def close(self):
        with self.lock:
            self.stream.close()


class Store:
    def __init__(self, db_path: Path, log: MonitorLog | None = None):
        db_path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(db_path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.Lock()
        self.log = log
        self.recent_messages = deque(maxlen=4096)
        self.recent_message_set = set()
        self.db.executescript(
            """
            CREATE TABLE IF NOT EXISTS devices (
                device TEXT PRIMARY KEY,
                role TEXT NOT NULL,
                last_seen REAL NOT NULL,
                reader_seen INTEGER,
                radio_ready INTEGER,
                packets INTEGER,
                gateway TEXT,
                heartbeat_seen REAL,
                hop_rssi INTEGER,
                hop_snr10 INTEGER,
                missed INTEGER,
                hop_age_ms INTEGER,
                relay_age_ms INTEGER,
                wifi_seen REAL,
                usb_seen REAL,
                wifi_ip TEXT,
                wifi_rssi INTEGER
            );
            CREATE TABLE IF NOT EXISTS tags (
                device TEXT NOT NULL,
                epc TEXT NOT NULL,
                first_seen REAL NOT NULL,
                last_seen REAL NOT NULL,
                active_since REAL,
                delay_since REAL,
                rssi INTEGER,
                lora_rssi INTEGER,
                relay_path TEXT NOT NULL DEFAULT '',
                active INTEGER NOT NULL,
                reads INTEGER NOT NULL,
                PRIMARY KEY (device, epc)
            );
            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                time REAL NOT NULL,
                device TEXT NOT NULL,
                event TEXT NOT NULL,
                epc TEXT NOT NULL,
                rssi INTEGER,
                lora_rssi INTEGER,
                gateway TEXT,
                relay_path TEXT NOT NULL DEFAULT '',
                dwell_seconds REAL
            );
            CREATE INDEX IF NOT EXISTS events_time ON events(time DESC);
            """
        )
        migrations = {
            "devices": {
                "heartbeat_seen": "REAL",
                "hop_rssi": "INTEGER",
                "hop_snr10": "INTEGER",
                "missed": "INTEGER",
                "hop_age_ms": "INTEGER",
                "relay_age_ms": "INTEGER",
                "wifi_seen": "REAL",
                "usb_seen": "REAL",
                "wifi_ip": "TEXT",
                "wifi_rssi": "INTEGER",
            },
            "tags": {
                "relay_path": "TEXT NOT NULL DEFAULT ''",
                "active_since": "REAL",
                "delay_since": "REAL",
            },
            "events": {
                "relay_path": "TEXT NOT NULL DEFAULT ''",
                "dwell_seconds": "REAL",
            },
        }
        for table, additions in migrations.items():
            columns = {row["name"] for row in self.db.execute(f"PRAGMA table_info({table})")}
            for column, definition in additions.items():
                if column not in columns:
                    self.db.execute(
                        f"ALTER TABLE {table} ADD COLUMN {column} {definition}"
                    )
        self.db.commit()

    def ingest(self, message: dict, transport: str = "usb") -> bool:
        if not isinstance(message, dict) or message.get("v") != 1:
            return False
        device = message.get("device")
        event = message.get("event")
        if not isinstance(device, str) or not DEVICE_RE.fullmatch(device):
            return False
        if event not in EVENTS:
            return False
        if transport not in {"usb", "wifi"}:
            return False
        epc = message.get("epc", "")
        if event in {"seen", "delayed", "lost"} and (
            not isinstance(epc, str) or not EPC_RE.fullmatch(epc)
        ):
            return False
        rssi = message.get("rssi")
        if event == "seen" and (type(rssi) is not int or not -128 <= rssi <= 127):
            return False
        lora_rssi = message.get("lora_rssi")
        if lora_rssi is not None and (
            type(lora_rssi) is not int or not -200 <= lora_rssi <= 20
        ):
            return False
        gateway = message.get("gateway")
        if gateway is not None and (
            not isinstance(gateway, str) or not DEVICE_RE.fullmatch(gateway)
        ):
            return False
        relay_path = message.get("relay_path", "")
        if not isinstance(relay_path, str) or (
            relay_path not in {"", "unknown"}
            and not re.fullmatch(r"[1-3](?:>[1-3])*", relay_path)
        ):
            return False
        boot_id = message.get("boot_id")
        msg_id = message.get("msg_id")
        if (boot_id is not None or msg_id is not None) and not (
            type(boot_id) is int and 0 <= boot_id <= 0xFFFFFFFF
            and type(msg_id) is int and 0 <= msg_id <= 0xFFFFFFFF
        ):
            return False

        now = time.time()
        with self.lock, self.db:
            message_key = (gateway or device, boot_id, msg_id) if boot_id is not None else None
            if message_key is not None:
                if message_key in self.recent_message_set:
                    if event == "gateway":
                        column = "wifi_seen" if transport == "wifi" else "usb_seen"
                        self.db.execute(
                            f"UPDATE devices SET {column}=? WHERE device=?", (now, device)
                        )
                    return event == "gateway"
                if len(self.recent_messages) == self.recent_messages.maxlen:
                    self.recent_message_set.discard(self.recent_messages.popleft())
                self.recent_messages.append(message_key)
                self.recent_message_set.add(message_key)
            if event == "gateway":
                wifi_ip = message.get("wifi_ip")
                if not isinstance(wifi_ip, str) or len(wifi_ip) > 45:
                    wifi_ip = None
                wifi_rssi = message.get("wifi_rssi")
                if type(wifi_rssi) is not int or not -200 <= wifi_rssi <= 0:
                    wifi_rssi = None
                self.db.execute(
                    """INSERT INTO devices(device,role,last_seen,radio_ready,packets,
                       relay_age_ms,wifi_seen,usb_seen,wifi_ip,wifi_rssi)
                       VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(device) DO UPDATE SET
                       role='gateway',last_seen=excluded.last_seen,
                       radio_ready=excluded.radio_ready,packets=excluded.packets,
                       relay_age_ms=excluded.relay_age_ms,
                       wifi_seen=COALESCE(excluded.wifi_seen,devices.wifi_seen),
                       usb_seen=COALESCE(excluded.usb_seen,devices.usb_seen),
                       wifi_ip=COALESCE(excluded.wifi_ip,devices.wifi_ip),
                       wifi_rssi=COALESCE(excluded.wifi_rssi,devices.wifi_rssi)""",
                    (
                        device,
                        "gateway",
                        now,
                        int(bool(message.get("radio_ready"))),
                        int(message.get("packets", 0)),
                        message.get("relay_age_ms"),
                        now if transport == "wifi" else None,
                        now if transport == "usb" else None,
                        wifi_ip,
                        wifi_rssi,
                    ),
                )
                return True

            is_direct_gateway = message.get("tag_source") == "direct_uhf" and device == gateway
            role = "gateway" if is_direct_gateway else "reader"
            self.db.execute(
                """INSERT INTO devices(device,role,last_seen,reader_seen,gateway)
                   VALUES(?,?,?,?,?) ON CONFLICT(device) DO UPDATE SET
                   role=excluded.role,last_seen=excluded.last_seen,
                   reader_seen=COALESCE(excluded.reader_seen,devices.reader_seen),
                   gateway=COALESCE(excluded.gateway,devices.gateway)""",
                (
                    device,
                    role,
                    now,
                    int(bool(message.get("reader_seen")))
                    if event == "heartbeat"
                    else None,
                    gateway,
                ),
            )
            if event == "heartbeat":
                self.db.execute(
                    """UPDATE devices SET heartbeat_seen=?,hop_rssi=?,hop_snr10=?,
                       missed=?,hop_age_ms=? WHERE device=?""",
                    (now, message.get("lora_rssi"), message.get("lora_snr10"),
                     message.get("missed"), message.get("hop_age_ms"), device),
                )
            if event == "seen":
                previous = self.db.execute(
                    "SELECT active,last_seen,active_since FROM tags WHERE device=? AND epc=?",
                    (device, epc),
                ).fetchone()
                is_new = previous is None or not previous["active"]
                active_since = (
                    now
                    if is_new or previous["active_since"] is None
                    else previous["active_since"]
                )
                self.db.execute(
                    """INSERT INTO tags(device,epc,first_seen,last_seen,active_since,delay_since,rssi,lora_rssi,relay_path,active,reads)
                       VALUES(?,?,?,?,?,NULL,?,?,?,1,1) ON CONFLICT(device,epc) DO UPDATE SET
                       last_seen=excluded.last_seen,rssi=excluded.rssi,
                       lora_rssi=excluded.lora_rssi,relay_path=excluded.relay_path,
                       active_since=excluded.active_since,delay_since=NULL,active=1,reads=reads+1""",
                    (device, epc, now, now, active_since, rssi, lora_rssi, relay_path),
                )
                if is_new:
                    self._event(now, device, event, epc, rssi, lora_rssi, gateway,
                                relay_path, reason="new")
            elif event == "delayed":
                self.db.execute(
                    """UPDATE tags SET delay_since=COALESCE(delay_since,?)
                       WHERE device=? AND epc=? AND active=1""",
                    (now, device, epc),
                )
            elif event == "lost":
                previous = self.db.execute(
                    "SELECT active,active_since,relay_path FROM tags WHERE device=? AND epc=?",
                    (device, epc),
                ).fetchone()
                if previous is not None and previous["active"]:
                    dwell_seconds = (
                        max(0.0, now - previous["active_since"])
                        if previous["active_since"] is not None
                        else None
                    )
                    self.db.execute(
                        "UPDATE tags SET active=0,active_since=NULL,delay_since=NULL WHERE device=? AND epc=?",
                        (device, epc),
                    )
                    self._event(
                        now, device, event, epc, None, lora_rssi, gateway,
                        previous["relay_path"] if relay_path == "unknown" else relay_path,
                        dwell_seconds, reason="explicit",
                    )
        return True

    def _event(
        self, now, device, event, epc, rssi, lora_rssi, gateway, relay_path,
        dwell_seconds=None, reason=None,
    ):
        self.db.execute(
            "INSERT INTO events(time,device,event,epc,rssi,lora_rssi,gateway,relay_path,dwell_seconds) "
            "VALUES(?,?,?,?,?,?,?,?,?)",
            (now, device, event, epc, rssi, lora_rssi, gateway, relay_path, dwell_seconds),
        )
        if self.log is not None:
            self.log.write("event", {"device": device, "event": event, "epc": epc,
                                     "reason": reason, "dwell_seconds": dwell_seconds,
                                     "relay_path": relay_path, "gateway": gateway})

    def state(self) -> dict:
        now = time.time()
        with self.lock, self.db:
            self._finalize_stale(now)
            devices = [
                dict(row)
                for row in self.db.execute("SELECT * FROM devices ORDER BY role,device")
            ]
            tags = [
                dict(row)
                for row in self.db.execute(
                    "SELECT * FROM tags ORDER BY device,active DESC,last_seen DESC"
                )
            ]
            events = [
                dict(row)
                for row in self.db.execute(
                    "SELECT * FROM events ORDER BY id DESC LIMIT 100"
                )
            ]
        for device in devices:
            if device["role"] == "gateway":
                wifi_live = bool(device["wifi_seen"] and now - device["wifi_seen"] < 5)
                usb_live = bool(device["usb_seen"] and now - device["usb_seen"] < 5)
                device["connection"] = (
                    "usb+wifi" if usb_live and wifi_live else
                    "wifi" if wifi_live else "usb" if usb_live else None
                )
            heartbeat_age = (
                now - device["heartbeat_seen"] if device["heartbeat_seen"] else None
            )
            hop_age = device["hop_age_ms"]
            device["heartbeat_online"] = bool(
                heartbeat_age is not None and heartbeat_age < 8
                and (hop_age is None or hop_age / 1000 + heartbeat_age < 8)
            )
            device["online"] = now - device["last_seen"] < 15
            device["tags"] = [
                tag for tag in tags if tag["device"] == device["device"]
            ]
            for tag in device["tags"]:
                tag["tag_source"] = (
                    "direct_uhf" if device["role"] == "gateway" else "lora_relay"
                )
                tag["active"] = bool(tag["active"])
                tag["visible"] = bool(
                    tag["active"] and now - tag["last_seen"] < TAG_DISPLAY_SECONDS
                )
                tag["delayed"] = bool(
                    tag["active"] and (
                        tag["delay_since"] is not None
                        or now - tag["last_seen"] >= TAG_DELAY_SECONDS
                    )
                )
        return {"devices": devices, "events": events, "now": now}

    def _finalize_stale(self, now: float) -> None:
        for tag in self.db.execute(
            """SELECT tags.*,devices.gateway FROM tags LEFT JOIN devices
               ON devices.device=tags.device WHERE tags.active=1 AND tags.last_seen<=?""",
            (now - TAG_EXPIRE_SECONDS,),
        ).fetchall():
            dwell_seconds = max(0.0, now - tag["active_since"]) if tag["active_since"] else None
            self.db.execute(
                "UPDATE tags SET active=0,active_since=NULL,delay_since=NULL WHERE device=? AND epc=?",
                (tag["device"], tag["epc"]),
            )
            self._event(
                now, tag["device"], "lost", tag["epc"], None,
                tag["lora_rssi"], tag["gateway"], tag["relay_path"], dwell_seconds,
                reason="timeout",
            )

    def export_csv(self) -> str:
        output = io.StringIO()
        writer = csv.writer(output)
        writer.writerow(["time_utc", "origin_reader", "event", "epc", "dwell_seconds", "rfid_rssi", "lora_rssi", "relay_path", "gateway"])
        with self.lock:
            for row in self.db.execute("SELECT * FROM events ORDER BY id"):
                writer.writerow(
                    [
                        time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(row["time"])),
                        row["device"],
                        row["event"],
                        row["epc"],
                        row["dwell_seconds"],
                        row["rssi"],
                        row["lora_rssi"],
                        row["relay_path"],
                        row["gateway"],
                    ]
                )
        return output.getvalue()


def forward_registration_event(sock: socket.socket, message: dict, accepted: bool,
                               log: MonitorLog, transport: str):
    if (not accepted or message.get("tag_source") != "direct_uhf"
            or message.get("device") != message.get("gateway")
            or message.get("event") not in {"seen", "lost", "delayed"}):
        return
    try:
        sock.sendto(
            json.dumps(message, separators=(",", ":")).encode("utf-8"),
            ("127.0.0.1", 8767),
        )
    except OSError as error:
        log.write("registration_forward", {
            "transport": transport,
            "event": "error",
            "detail": str(error),
            "epc": message.get("epc"),
        })


def receiver_lines(store: Store, port_name: str, stop: threading.Event, log: MonitorLog):
    registration_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    while not stop.is_set():
        ports = (
            [port_name]
            if port_name != "auto"
            else [port.device for port in list_ports.comports()]
        )
        for port in ports:
            if stop.is_set():
                break
            try:
                with serial.Serial(port, 115200, timeout=1) as connection:
                    print(f"Checking receiver on {port}", flush=True)
                    identified = False
                    deadline = time.monotonic() + 8
                    while not stop.is_set():
                        if port_name == "auto" and not identified and time.monotonic() > deadline:
                            break
                        raw = connection.readline()
                        if not raw:
                            continue
                        try:
                            message = json.loads(raw.decode("utf-8").strip())
                        except (UnicodeDecodeError, json.JSONDecodeError):
                            continue
                        if not identified:
                            if message.get("event") != "gateway":
                                continue
                            identified = True
                            print(f"Receiver found: {message.get('device')} on {port}", flush=True)
                        accepted = store.ingest(message)
                        log.write("receiver_usb", {"port": port, "accepted": accepted,
                                                   "message": message})
                        forward_registration_event(
                            registration_socket, message, accepted, log, "usb"
                        )
            except (serial.SerialException, OSError) as error:
                if port_name != "auto":
                    print(f"Waiting for {port}: {error}", flush=True)
        stop.wait(2)
    registration_socket.close()


def reader_lines(port: str, stop: threading.Event, log: MonitorLog):
    last_error = None
    while not stop.is_set():
        try:
            with serial.Serial(port, 115200, timeout=1) as connection:
                log.write("reader_serial", {"port": port, "event": "connected"})
                last_error = None
                while not stop.is_set():
                    raw = connection.readline()
                    if raw:
                        log.write("reader_serial", {"port": port,
                                                    "line": raw.decode("utf-8", "replace").strip()[:512]})
        except (serial.SerialException, OSError) as error:
            current_error = str(error)
            if current_error != last_error:
                log.write("reader_serial", {"port": port, "event": "error",
                                            "detail": current_error})
                last_error = current_error
        stop.wait(2)


def wifi_lines(store: Store, port: int, stop: threading.Event, log: MonitorLog):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection, \
                socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as registration_socket:
            connection.bind(("0.0.0.0", port))
            connection.settimeout(1)
            print(f"Wi-Fi receiver listening on UDP {port}", flush=True)
            while not stop.is_set():
                try:
                    raw, _address = connection.recvfrom(2048)
                except socket.timeout:
                    continue
                if len(raw) > 1024:
                    continue
                try:
                    message = json.loads(raw.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                accepted = store.ingest(message, transport="wifi")
                log.write("wifi", {"accepted": accepted, "message": message})
                forward_registration_event(
                    registration_socket, message, accepted, log, "wifi"
                )
    except OSError as error:
        print(f"Wi-Fi receiver unavailable: {error}", flush=True)
        log.write("system", {"event": "wifi_error", "detail": str(error)})


def snapshot_lines(store: Store, stop: threading.Event, log: MonitorLog):
    while not stop.wait(5):
        state = store.state()
        devices = []
        for device in state["devices"]:
            if device["device"] not in {
                "playce-ca9bd0", "playce-92f6ac", "playce-ca3490", "playce-cb21f8"
            }:
                continue
            devices.append({
                "device": device["device"],
                "online": device["online"],
                "connection": device.get("connection"),
                "reader_seen": device["reader_seen"],
                "heartbeat_online": device["heartbeat_online"],
                "last_seen_age_s": round(state["now"] - device["last_seen"], 1),
                "heartbeat_age_s": round(state["now"] - device["heartbeat_seen"], 1)
                if device["heartbeat_seen"] else None,
                "hop_rssi": device["hop_rssi"],
                "hop_snr10": device["hop_snr10"],
                "missed": device["missed"],
                "hop_age_ms": device["hop_age_ms"],
                "packets": device["packets"],
                "tags": [{
                    "epc": tag["epc"], "delayed": tag["delayed"],
                    "last_seen_age_s": round(state["now"] - tag["last_seen"], 1),
                    "dwell_s": round(state["now"] - tag["active_since"], 1)
                    if tag["active_since"] else None,
                    "relay_path": tag["relay_path"],
                } for tag in device["tags"] if tag["active"]],
            })
        log.write("snapshot", {"devices": devices})


def make_handler(store: Store):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path in STATIC_FILES:
                relative_path, mime = STATIC_FILES[self.path]
                content = (HERE / relative_path).read_bytes()
            elif self.path == "/api/state":
                content = json.dumps(store.state(), ensure_ascii=False).encode("utf-8")
                mime = "application/json; charset=utf-8"
            elif self.path == "/api/export.csv":
                content = store.export_csv().encode("utf-8-sig")
                mime = "text/csv; charset=utf-8"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(content)

        def log_message(self, format, *args):
            pass

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="auto", help="Receiver COM port, or auto")
    parser.add_argument("--wifi-port", type=int, default=8766)
    parser.add_argument("--web-port", type=int, default=8765)
    parser.add_argument("--reader-port", default="off", help="Optional reader serial port for log.txt")
    parser.add_argument("--note", help="Append a debugging observation to log.txt and exit")
    parser.add_argument("--note-kind", choices=("observation", "inference", "action", "verification"),
                        default="observation")
    parser.add_argument("--evidence", action="append", default=[])
    args = parser.parse_args()
    log = MonitorLog(LOG_PATH)
    if args.note is not None:
        log.write("analysis", {"kind": args.note_kind, "summary": args.note,
                               "evidence": args.evidence})
        log.close()
        return
    store = Store(DATA_DIR / "events.sqlite3", log)
    log.write("system", {"event": "start", "receiver_port": args.port,
                         "reader_port": args.reader_port, "wifi_port": args.wifi_port})
    stop = threading.Event()
    workers = [
        threading.Thread(target=wifi_lines, args=(store, args.wifi_port, stop, log), daemon=True),
        threading.Thread(target=snapshot_lines, args=(store, stop, log), daemon=True),
    ]
    if args.port != "off":
        workers.append(
            threading.Thread(target=receiver_lines, args=(store, args.port, stop, log), daemon=True)
        )
    if args.reader_port != "off":
        workers.append(
            threading.Thread(target=reader_lines, args=(args.reader_port, stop, log), daemon=True)
        )
    for worker in workers:
        worker.start()
    server = ThreadingHTTPServer(("127.0.0.1", args.web_port), make_handler(store))
    print(f"Dashboard: http://127.0.0.1:{args.web_port}/", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.server_close()
        for worker in workers:
            worker.join(timeout=3)
        store.db.close()
        log.write("system", {"event": "stop"})
        log.close()


if __name__ == "__main__":
    main()
