import http.client
import json
import socket
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import SimpleNamespace
from unittest.mock import MagicMock, patch
from urllib.error import HTTPError, URLError

import uplink
from uplink import Uplink, check_backend_url, forward_registration, parse_line, serial_lines, wifi_lines

GATEWAY = "playce-ca9bd0"
EPC = "E28069150000401D63E439A0"


def quiet(*_args, **_kwargs):
    pass


class FakeResponse:
    def __init__(self, payload):
        self.payload = payload

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self):
        return json.dumps(self.payload).encode("utf-8")


class FakeClock:
    def __init__(self, now=100.0):
        self.now = now

    def __call__(self):
        return self.now


def recording_opener(payload=None, before=None):
    requests = []

    def opener(request, timeout):
        requests.append(request)
        if before:
            before(len(requests))
        return FakeResponse(payload if payload is not None else {"accepted": 1})

    return opener, requests


def sent(requests):
    return [(json.loads(request.data)["transport"], json.loads(request.data)["messages"]) for request in requests]


class ParseAndForwardTest(unittest.TestCase):
    def test_parse_line_accepts_only_json_objects(self):
        self.assertEqual(parse_line(b'{"v":1,"event":"gateway"}\r\n'), {"v": 1, "event": "gateway"})
        self.assertEqual(parse_line('{"v":1}'), {"v": 1})
        self.assertIsNone(parse_line(b"TAG_DETECTED,E280,-40\n"))
        self.assertIsNone(parse_line(b"[1,2]"))
        self.assertIsNone(parse_line(b"\xff\xfe"))

    def test_only_valid_direct_receiver_reads_go_to_registration(self):
        sock = MagicMock()
        direct = {"v": 1, "device": GATEWAY, "gateway": GATEWAY, "event": "seen", "epc": EPC, "tag_source": "direct_uhf"}
        for message in (
            direct,
            {**direct, "device": "playce-92f6ac", "tag_source": "lora_relay"},
            {**direct, "event": "heartbeat"},
            {**direct, "gateway": "playce-000000"},
            {**direct, "epc": EPC.lower()},  # the backend would reject it, so /qr could never show its challenge
            {**direct, "v": 2},
            {**direct, "device": "PLAYCE-CA9BD0", "gateway": "PLAYCE-CA9BD0"},
        ):
            forward_registration(sock, message)
        sock.sendto.assert_called_once()
        payload, address = sock.sendto.call_args.args
        self.assertEqual(json.loads(payload), direct)
        self.assertEqual(address, ("127.0.0.1", 8767))

    def test_backend_url_must_be_https_except_for_this_machine(self):
        self.assertEqual(check_backend_url("https://playce-admin.mooo.com/"), "https://playce-admin.mooo.com")
        self.assertEqual(check_backend_url("http://127.0.0.1:4300"), "http://127.0.0.1:4300")
        self.assertEqual(check_backend_url("http://localhost:4300"), "http://localhost:4300")
        for url in ("http://playce-admin.mooo.com", "http://192.168.0.10:4300", "ftp://x", "playce-admin.mooo.com"):
            with self.assertRaises(ValueError, msg=url):
                check_backend_url(url)


class UplinkSendTest(unittest.TestCase):
    def test_keeps_arrival_order_across_transports_with_the_internal_token(self):
        opener, requests = recording_opener()
        link = Uplink("https://backend.test/", "secret", log=quiet, opener=opener, clock=FakeClock())
        link.send_batch([(100.0, "usb", {"n": 1}), (100.0, "usb", {"n": 2}), (100.0, "wifi", {"n": 3}),
                         (100.0, "usb", {"n": 4})])
        self.assertEqual(sent(requests), [("usb", [{"n": 1}, {"n": 2}]), ("wifi", [{"n": 3}]), ("usb", [{"n": 4}])])
        first = requests[0]
        self.assertEqual(first.full_url, "https://backend.test/internal/rfid/events")
        self.assertEqual(first.get_method(), "POST")
        self.assertEqual(first.get_header("X-khuround-internal-token"), "secret")
        self.assertEqual(link.stats["sent"], 4)

    def test_full_requests_stay_under_the_backend_body_and_count_limits(self):
        opener, requests = recording_opener()
        link = Uplink("https://backend.test", "secret", log=quiet, opener=opener, clock=FakeClock())
        messages = [{"v": 1, "device": GATEWAY, "event": "gateway", "pad": "x" * 900} for _ in range(uplink.BATCH_MAX_MESSAGES)]
        link.send_batch([(100.0, "usb", message) for message in messages])
        self.assertGreater(len(requests), 1)
        self.assertTrue(all(len(request.data) < 64 * 1024 for request in requests))
        self.assertTrue(all(len(json.loads(request.data)["messages"]) <= 200 for request in requests))
        self.assertEqual(sum(len(json.loads(request.data)["messages"]) for request in requests), len(messages))

    def test_staleness_is_judged_right_before_each_post(self):
        clock = FakeClock(100.0)

        def slow_first_post(count):
            if count == 1:
                clock.now += uplink.STALE_SECONDS + 1  # the first POST took longer than the stale limit

        opener, requests = recording_opener(before=slow_first_post)
        link = Uplink("https://backend.test", "secret", log=quiet, opener=opener, clock=clock)
        link.send_batch([(100.0, "usb", {"n": 1}), (100.0, "wifi", {"n": 2})])
        self.assertEqual(sent(requests), [("usb", [{"n": 1}])])
        self.assertEqual(link.stats["dropped"], 1)

    def test_a_fast_failure_is_retried_once_while_lines_are_fresh(self):
        outcomes = [URLError("refused"), None]
        requests = []

        def opener(request, timeout):
            requests.append(request)
            error = outcomes.pop(0)
            if error:
                raise error
            return FakeResponse({"accepted": 1})

        link = Uplink("https://backend.test", "secret", log=quiet, opener=opener, clock=FakeClock())
        link.send_batch([(100.0, "usb", {"n": 1})])
        self.assertEqual(len(requests), 2)
        self.assertEqual(link.stats, {"sent": 1, "accepted": 1, "dropped": 0, "failed_batches": 1})

    def test_failures_are_reported_once_never_raise_and_drop_after_one_retry(self):
        logs = []
        errors = [HTTPError("https://backend.test", 401, "Unauthorized", {}, None) for _ in range(2)]
        errors += [URLError("down"), http.client.IncompleteRead(b"partial")]

        def opener(_request, timeout):
            raise errors.pop(0)

        link = Uplink("https://backend.test", "bad", log=lambda text, **_: logs.append(text), opener=opener, clock=FakeClock())
        link.send_batch([(100.0, "usb", {"n": 1})])
        link.send_batch([(100.0, "usb", {"n": 2})])
        self.assertEqual(logs[:2], ["backend HTTP 401 (check KHUROUND_RFID_UPLINK_TOKEN)", "backend unreachable: down"])
        self.assertTrue(logs[2].startswith("backend unreachable: IncompleteRead"))
        self.assertEqual(link.stats["failed_batches"], 4)
        self.assertEqual(link.stats["dropped"], 2)

    def test_an_odd_success_body_does_not_break_the_sender(self):
        opener, _ = recording_opener(payload={"accepted": None})
        link = Uplink("https://backend.test", "secret", log=quiet, opener=opener, clock=FakeClock())
        self.assertTrue(link.post("usb", [{"n": 1}]))
        self.assertEqual(link.stats["accepted"], 0)

    def test_sender_thread_survives_unexpected_errors_and_flushes_on_stop(self):
        opener, requests = recording_opener()
        logs = []
        link = Uplink("https://backend.test", "secret", log=lambda text, **_: logs.append(text), opener=opener)
        real_drain = link.drain
        calls = {"n": 0}

        def flaky_drain(wait=0.5):
            calls["n"] += 1
            if calls["n"] == 1:
                raise RuntimeError("boom")
            return real_drain(wait)

        link.drain = flaky_drain
        link.put({"n": 1}, "usb")
        stop = threading.Event()
        stop.set()
        link.run(stop)
        self.assertTrue(logs[0].startswith("Uplink sender error: RuntimeError"))
        self.assertEqual(sent(requests), [("usb", [{"n": 1}])])

    def test_redirects_are_refused_so_the_token_never_follows_them(self):
        hits = []

        class Redirecting(BaseHTTPRequestHandler):
            def do_POST(self):
                hits.append(self.path)
                self.send_response(302)
                self.send_header("Location", "http://127.0.0.1:9/elsewhere")
                self.end_headers()

            def log_message(self, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Redirecting)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            logs = []
            link = Uplink(f"http://127.0.0.1:{server.server_address[1]}", "secret", log=lambda text, **_: logs.append(text))
            self.assertFalse(link.post("usb", [{"n": 1}]))
            self.assertEqual(hits, ["/internal/rfid/events"])
            self.assertEqual(logs, ["backend HTTP 302"])
        finally:
            server.shutdown()
            server.server_close()


class InputTest(unittest.TestCase):
    def test_auto_probe_forwards_lines_once_the_receiver_identifies_itself(self):
        lines = iter([
            b"boot noise\n",
            b'{"v":1,"device":"playce-92f6ac","event":"heartbeat"}\n',
            b'{"v":1,"device":"playce-ca9bd0","event":"gateway"}\n',
            b'{"v":1,"device":"playce-92f6ac","event":"seen"}\n',
        ])

        class Port:
            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def readline(self):
                try:
                    return next(lines)
                except StopIteration:
                    stop.set()
                    return b""

        stop = threading.Event()
        received = []
        with patch("uplink.list_ports.comports", return_value=[SimpleNamespace(device="/dev/ttyUSB0")]), \
                patch("uplink.serial.Serial", return_value=Port()) as opened:
            serial_lines("auto", stop, lambda message, transport: received.append((message["event"], transport)), log=quiet)
        opened.assert_called_with("/dev/ttyUSB0", 115200, timeout=1)
        self.assertEqual(received, [("gateway", "usb"), ("seen", "usb")])

    def test_wifi_lines_only_from_allowed_receivers(self):
        stop = threading.Event()
        packets = iter([
            (b'{"v":1,"n":1}', ("192.168.0.50", 4000)),
            (b'{"v":1,"n":2}', ("192.168.0.66", 4000)),
            (b"x" * 1500, ("192.168.0.50", 4000)),
            (b'{"v":1,"n":3}', ("192.168.0.50", 4000)),
        ])

        class FakeSocket:
            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def bind(self, address):
                self.address = address

            def settimeout(self, _seconds):
                pass

            def recvfrom(self, _size):
                try:
                    return next(packets)
                except StopIteration:
                    stop.set()
                    raise socket.timeout()

        received = []
        with patch("uplink.socket.socket", return_value=FakeSocket()):
            wifi_lines(8766, stop, lambda message, transport: received.append((message["n"], transport)),
                       allow={"192.168.0.50"}, log=quiet)
        self.assertEqual(received, [(1, "wifi"), (3, "wifi")])


if __name__ == "__main__":
    unittest.main()
