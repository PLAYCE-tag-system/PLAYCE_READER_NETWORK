import io
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from collector import MonitorLog, Store, make_handler, receiver_lines


DEVICE = "playce-92f6ac"
EPC = "E28069150000401D63E439A0"


class ReceiverProbeTest(unittest.TestCase):
    def test_auto_probe_releases_busy_reader_output_after_deadline(self):
        class StopAfterProbe:
            stopped = False

            def is_set(self):
                return self.stopped

            def wait(self, _seconds):
                self.stopped = True

        class ContinuousReader:
            lines = 0

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def readline(self):
                self.lines += 1
                return b"TAG_DETECTED,E28069150000401D63E439A0,-40\n"

        reader = ContinuousReader()
        stop = StopAfterProbe()
        with patch("collector.list_ports.comports", return_value=[SimpleNamespace(device="COM9")]), \
                patch("collector.serial.Serial", return_value=reader), \
                patch("collector.socket.socket"), \
                patch("collector.time.monotonic", side_effect=iter(range(20))):
            receiver_lines(None, "auto", stop, None)
        self.assertTrue(stop.stopped)
        self.assertEqual(reader.lines, 8)


def message(event):
    data = {"v": 1, "device": DEVICE, "event": event, "epc": EPC,
            "gateway": "playce-ca9bd0", "relay_path": "1>2>3"}
    if event == "seen":
        data["rssi"] = -45
    return data


class DelayStateTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.directory.name) / "events.sqlite3")

    def tearDown(self):
        self.store.db.close()
        self.directory.cleanup()

    def tag(self):
        state = self.store.state()
        device = next(item for item in state["devices"] if item["device"] == DEVICE)
        return device["tags"][0], state["events"]

    def test_gap_and_recovery_keep_dwell_start(self):
        with patch("collector.time.time", return_value=1000):
            self.assertTrue(self.store.ingest(message("seen")))
        with patch("collector.time.time", return_value=1006):
            tag, events = self.tag()
            self.assertTrue(tag["active"])
            self.assertTrue(tag["delayed"])
            self.assertEqual(tag["active_since"], 1000)
            self.assertEqual(len(events), 1)
        with patch("collector.time.time", return_value=1012):
            self.assertTrue(self.store.ingest(message("delayed")))
            tag, _ = self.tag()
            self.assertTrue(tag["delayed"])
        with patch("collector.time.time", return_value=1015):
            self.assertTrue(self.store.ingest(message("seen")))
            tag, events = self.tag()
            self.assertFalse(tag["delayed"])
            self.assertEqual(tag["active_since"], 1000)
            self.assertEqual(len(events), 1)

    def test_delay_is_visible_for_two_seconds_before_live_tag_hides(self):
        with patch("collector.time.time", return_value=1000):
            self.store.ingest(message("seen"))
        with patch("collector.time.time", return_value=1004.9):
            tag, _ = self.tag()
            self.assertTrue(tag["visible"])
            self.assertFalse(tag["delayed"])
        with patch("collector.time.time", return_value=1005):
            tag, events = self.tag()
            self.assertTrue(tag["visible"])
            self.assertTrue(tag["delayed"])
            self.assertTrue(tag["active"])
            self.assertEqual(len(events), 1)
        with patch("collector.time.time", return_value=1006.9):
            tag, _ = self.tag()
            self.assertTrue(tag["visible"])
            self.assertTrue(tag["delayed"])
        with patch("collector.time.time", return_value=1007):
            tag, events = self.tag()
            self.assertFalse(tag["visible"])
            self.assertTrue(tag["active"])
            self.assertEqual(len(events), 1)
        with patch("collector.time.time", return_value=1011):
            self.store.ingest(message("seen"))
            tag, events = self.tag()
            self.assertTrue(tag["visible"])
            self.assertFalse(tag["delayed"])
            self.assertEqual(tag["active_since"], 1000)
            self.assertEqual(len(events), 1)

    def test_explicit_lost_ends_dwell(self):
        with patch("collector.time.time", return_value=1000):
            self.store.ingest(message("seen"))
        with patch("collector.time.time", return_value=1004):
            self.store.ingest(message("lost"))
            tag, events = self.tag()
            self.assertFalse(tag["active"])
            self.assertEqual(events[0]["dwell_seconds"], 4)

    def test_unknown_path_lost_uses_previous_route(self):
        with patch("collector.time.time", return_value=1000):
            self.store.ingest(message("seen"))
        lost = message("lost")
        lost["relay_path"] = "unknown"
        with patch("collector.time.time", return_value=1004):
            self.assertTrue(self.store.ingest(lost))
            tag, events = self.tag()
            self.assertFalse(tag["active"])
            self.assertEqual(events[0]["event"], "lost")
            self.assertEqual(events[0]["dwell_seconds"], 4)
            self.assertEqual(events[0]["relay_path"], "1>2>3")

    def test_long_gap_finalizes_once(self):
        with patch("collector.time.time", return_value=1000):
            self.store.ingest(message("seen"))
        with patch("collector.time.time", return_value=1031):
            tag, events = self.tag()
            self.assertFalse(tag["active"])
            self.assertEqual(events[0]["event"], "lost")
            self.assertEqual(events[0]["dwell_seconds"], 31)
            self.assertEqual(len(events), 2)
            self.assertEqual(len(self.store.state()["events"]), 2)

    def test_recent_tag_keeps_reader_online_when_heartbeat_is_late(self):
        with patch("collector.time.time", return_value=1000):
            self.store.ingest({"v": 1, "device": DEVICE, "event": "heartbeat",
                               "reader_seen": True, "hop_age_ms": 0})
        with patch("collector.time.time", return_value=1018):
            self.store.ingest(message("seen"))
        with patch("collector.time.time", return_value=1020):
            state = self.store.state()
            device = next(item for item in state["devices"] if item["device"] == DEVICE)
            self.assertTrue(device["online"])
            self.assertFalse(device["heartbeat_online"])


class MonitorLogTest(unittest.TestCase):
    def test_log_appends_json_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.txt"
            for value in (1, 2):
                log = MonitorLog(path)
                log.write("analysis", {"kind": "observation", "value": value})
                log.close()
            import json
            entries = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
            self.assertEqual([entry["value"] for entry in entries], [1, 2])
            self.assertTrue(all(entry["source"] == "analysis" for entry in entries))

    def test_store_logs_derived_events(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.txt"
            log = MonitorLog(path)
            store = Store(Path(directory) / "events.sqlite3", log)
            with patch("collector.time.time", return_value=1000):
                store.ingest(message("seen"))
            with patch("collector.time.time", return_value=1003):
                store.ingest(message("lost"))
            store.db.close()
            log.close()
            import json
            entries = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
            self.assertEqual([entry["reason"] for entry in entries], ["new", "explicit"])


class DashboardHttpTest(unittest.TestCase):
    def test_dashboard_assets_and_api(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Store(Path(directory) / "events.sqlite3")
            try:
                for path, mime in (
                    ("/", "text/html"),
                    ("/dashboard.css", "text/css"),
                    ("/dashboard.js", "text/javascript"),
                    ("/fonts/Freesentation-9Black.woff2", "font/woff2"),
                    ("/api/state", "application/json"),
                    ("/api/export.csv", "text/csv"),
                ):
                    with self.subTest(path=path):
                        status, headers, body = self._request(store, path)
                        self.assertEqual(status, 200)
                        self.assertIn(mime, headers["Content-Type"])
                        self.assertTrue(body)
                status, _, _ = self._request(store, "/missing.js")
                self.assertEqual(status, 404)
            finally:
                store.db.close()

    @staticmethod
    def _request(store, path):
        handler = object.__new__(make_handler(store))
        handler.path = path
        handler.wfile = io.BytesIO()
        result = {"status": None, "headers": {}}
        handler.send_response = lambda status: result.update(status=status)
        handler.send_header = lambda name, value: result["headers"].update({name: value})
        handler.end_headers = lambda: None
        handler.send_error = lambda status: result.update(status=status)
        handler.do_GET()
        return result["status"], result["headers"], handler.wfile.getvalue()


if __name__ == "__main__":
    unittest.main()
