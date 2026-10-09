"""Tests that do not require a Home Assistant installation."""

import ast
import importlib.util
import json
from pathlib import Path
import unittest


PROTOCOL_PATH = Path(__file__).resolve().parents[1] / "custom_components/fusionedge/protocol.py"
SPEC = importlib.util.spec_from_file_location("fusionedge_protocol_test", PROTOCOL_PATH)
assert SPEC and SPEC.loader
protocol = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(protocol)


class FusionEdgeProtocolTest(unittest.TestCase):
    def test_auto_timer_accepts_24_hour_time_or_off(self) -> None:
        for value in ("00:00", "07:30", "23:59", "off"):
            self.assertTrue(protocol.valid_timer_value(value))
        for value in ("", "24:00", "7:30", "12:60", "12:30 ", "12:30\n", None):
            self.assertFalse(protocol.valid_timer_value(value))

    def test_led_effect_names_keep_firmware_indices(self) -> None:
        self.assertEqual(
            protocol.LED_EFFECTS,
            ("Standard Stereo", "Rainbow Flow", "Twinkle", "Streamline"),
        )

    def test_playlist_station_ids_skip_only_invalid_csv_rows(self) -> None:
        data = (
            ".Első rádió\thttp://radio.test/1\t0\t\n"
            "invalid line\n"
            "\thttp://radio.test/hidden\t0\n"
            "Második\thttp://radio.test/2\t0\n"
            "No volume\thttp://radio.test/3\t\n"
            "Harmadik\thttp://radio.test/4\t0\r\n"
        ).encode("utf-8")
        self.assertEqual(
            protocol.parse_station_playlist(data),
            [(1, ".Első rádió"), (2, "Második"), (3, "Harmadik")],
        )

    def test_playlist_url_is_the_local_edge_endpoint(self) -> None:
        self.assertTrue(
            protocol.valid_playlist_url("http://10.0.0.42/data/playlist.csv")
        )
        self.assertTrue(
            protocol.valid_playlist_url("http://192.168.1.42/data/playlist.csv")
        )
        for url in (
            "https://10.0.0.42/data/playlist.csv",
            "http://127.0.0.1/data/playlist.csv",
            "http://169.254.169.254/data/playlist.csv",
            "http://8.8.8.8/data/playlist.csv",
            "http://10.0.0.42/private",
            "http://10.0.0.42/data/playlist.csv?secret=1",
            "http://10.0.0.42/data/playlist.csv\n",
        ):
            with self.subTest(url=url):
                self.assertFalse(protocol.valid_playlist_url(url))

    def test_sources_match_state_and_command_contract(self) -> None:
        pairs = {
            "web": ("WEB", "mode web"),
            "sd": ("SD", "mode sd"),
            "dlna": ("DLNA", "mode dlna"),
            "bt_input": ("Bluetooth input", "mode bt"),
        }
        self.assertEqual(set(protocol.SOURCES), {label for label, _ in pairs.values()})
        for mode, (label, command) in pairs.items():
            with self.subTest(mode=mode):
                self.assertEqual(protocol.source_from_mode(mode), label)
                self.assertEqual(protocol.command_for_source(label), command)
        self.assertIsNone(protocol.source_from_mode("source"))
        self.assertIsNone(protocol.command_for_source("mode bt"))

    def test_mqtt_subscriptions_are_awaited(self) -> None:
        """HA's async_subscribe returns an unsubscribe callback only when awaited."""
        source = PROTOCOL_PATH.with_name("media_player.py").read_text(encoding="utf-8")
        tree = ast.parse(source)
        calls = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "async_subscribe"
        ]
        awaited = [
            node.value
            for node in ast.walk(tree)
            if isinstance(node, ast.Await) and isinstance(node.value, ast.Call)
        ]
        self.assertEqual(len(calls), 1)
        self.assertIn(calls[0], awaited)

    def test_discovery_accepts_matching_mac_and_root(self) -> None:
        data = protocol.parse_discovery(
            "fusionedge/discovery/94A990D248B4/config",
            json.dumps(
                {
                    "id": "94A990D248B4",
                    "name": "FusionEdge-48B4",
                    "root_topic": "fusionedge/radio1/",
                    "sw_version": "2.0.0",
                }
            ),
        )
        self.assertEqual(data["device_id"], "94A990D248B4")
        self.assertEqual(data["root_topic"], "fusionedge/radio1/")

    def test_discovery_rejects_wrong_identity_or_topic(self) -> None:
        payload = '{"id":"94A990D248B4","root_topic":"fusionedge/radio1/"}'
        self.assertIsNone(
            protocol.parse_discovery("fusionedge/discovery/AAAAAAAAAAAA/config", payload)
        )
        self.assertIsNone(protocol.parse_discovery("homeassistant/sensor/x/config", payload))
        self.assertIsNone(protocol.parse_discovery("fusionedge/discovery/94A990D248B4/config", "{"))

    def test_discovery_rejects_unsafe_root(self) -> None:
        topic = "fusionedge/discovery/94A990D248B4/config"
        for root in ("fusionedge/radio1", "fusionedge/+/", "fusionedge/#/", ""):
            with self.subTest(root=root):
                self.assertIsNone(
                    protocol.parse_discovery(
                        topic, json.dumps({"id": "94A990D248B4", "root_topic": root})
                    )
                )

    def test_volume_is_integer_step_not_percent(self) -> None:
        self.assertEqual(protocol.parse_volume("0"), 0)
        self.assertEqual(protocol.parse_volume("21"), 21)
        for payload in ("22", "35", "-1", "5.5", "NaN", ""):
            with self.subTest(payload=payload):
                self.assertIsNone(protocol.parse_volume(payload))

        self.assertEqual(protocol.volume_to_step(0), 0)
        self.assertEqual(protocol.volume_to_step(0.5), 11)
        self.assertEqual(protocol.volume_to_step(1), 21)
        self.assertIsNone(protocol.volume_to_step(35))

    def test_status_requires_json_object(self) -> None:
        self.assertEqual(protocol.parse_json_object('{"playing":1}'), {"playing": 1})
        self.assertIsNone(protocol.parse_json_object("[]"))
        self.assertIsNone(protocol.parse_json_object("oops"))


if __name__ == "__main__":
    unittest.main()
