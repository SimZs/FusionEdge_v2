"""Small, Home-Assistant-independent validation of FusionEdge MQTT messages."""

import json
from ipaddress import IPv4Address, IPv4Network
import re
from typing import Any
from urllib.parse import urlsplit

DISCOVERY_PREFIX = "fusionedge/discovery/"
MAX_VOLUME = 21
MAX_PLAYLIST_BYTES = 512 * 1024
MAX_BROWSE_STATIONS = 2000
PLAYLIST_MODES = frozenset(("web", "sd", "dlna"))
BT_OUTPUT_OPTIONS = ("Bluetooth only", "Local + Bluetooth", "Local only")
LED_EFFECTS = ("Standard Stereo", "Rainbow Flow", "Twinkle", "Streamline")
TIMER_FIELDS = ("weekday_start", "weekday_stop", "weekend_start", "weekend_stop")
TIMER_PATTERN = r"(?:off|(?:[01][0-9]|2[0-3]):[0-5][0-9])"
_TIMER_TIME = re.compile(r"(?:[01][0-9]|2[0-3]):[0-5][0-9]\Z")
_DEVICE_ID = re.compile(r"[0-9A-F]{12}\Z")
_LOCAL_IPV4_NETWORKS = (
    IPv4Network("10.0.0.0/8"),
    IPv4Network("172.16.0.0/12"),
    IPv4Network("192.168.0.0/16"),
)

# The values in the state topic differ from the firmware's command tokens.
_MODE_TO_SOURCE = {
    "web": "WEB",
    "sd": "SD",
    "dlna": "DLNA",
    "bt_input": "Bluetooth input",
}
_SOURCE_TO_COMMAND = {
    "WEB": "mode web",
    "SD": "mode sd",
    "DLNA": "mode dlna",
    "Bluetooth input": "mode bt",
}
SOURCES = tuple(_SOURCE_TO_COMMAND)


def valid_timer_value(value: object) -> bool:
    """Only a 24-hour HH:MM time or the explicit disabled value is accepted."""
    return isinstance(value, str) and (value == "off" or bool(_TIMER_TIME.fullmatch(value)))


def source_from_mode(mode: object) -> str | None:
    """Convert an actual Edge mode into a Home Assistant source label."""
    return _MODE_TO_SOURCE.get(mode) if isinstance(mode, str) else None


def command_for_source(source: str) -> str | None:
    """Return only known, validated Edge mode commands."""
    return _SOURCE_TO_COMMAND.get(source)


def parse_discovery(topic: str, payload: str) -> dict[str, str] | None:
    """Accept only this integration's matching device identity and MQTT root."""
    if not topic.startswith(DISCOVERY_PREFIX) or not topic.endswith("/config"):
        return None
    topic_id = topic[len(DISCOVERY_PREFIX) : -len("/config")]
    if not _DEVICE_ID.fullmatch(topic_id) or topic_id == "000000000000":
        return None
    try:
        data = json.loads(payload)
    except (TypeError, ValueError):
        return None
    if not isinstance(data, dict) or data.get("id") != topic_id:
        return None
    root = data.get("root_topic")
    if (
        not isinstance(root, str)
        or not root
        or len(root) > 80
        or not root.endswith("/")
        or any(ch in root for ch in ("+", "#", "\0"))
        or any(ord(ch) < 0x20 for ch in root)
    ):
        return None
    name = data.get("name")
    version = data.get("sw_version")
    return {
        "device_id": topic_id,
        "root_topic": root,
        "name": name[:80] if isinstance(name, str) and name else f"FusionEdge-{topic_id[-4:]}",
        "sw_version": version[:40] if isinstance(version, str) else "",
    }


def parse_json_object(payload: str) -> dict[str, Any] | None:
    """Ignore malformed or non-object status publications."""
    try:
        data = json.loads(payload)
    except (TypeError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def parse_volume(payload: str) -> int | None:
    """Return an Edge volume step, never an out-of-range percentage."""
    try:
        value = int(payload)
    except (TypeError, ValueError):
        return None
    return value if 0 <= value <= MAX_VOLUME else None


def volume_to_step(level: float) -> int | None:
    """Convert HA's normalized volume to the integer command range."""
    if not isinstance(level, (int, float)) or not 0 <= level <= 1:
        return None
    return int(level * MAX_VOLUME + 0.5)


def valid_playlist_url(url: object) -> bool:
    """Only fetch the Edge's fixed playlist endpoint on a LAN address."""
    if (
        not isinstance(url, str)
        or len(url) > 200
        or url != url.strip()
        or any(ord(char) < 0x20 for char in url)
    ):
        return False
    try:
        parts = urlsplit(url)
        address = IPv4Address(parts.hostname or "")
        port = parts.port
    except ValueError:
        return False
    return (
        parts.scheme == "http"
        and any(address in network for network in _LOCAL_IPV4_NETWORKS)
        and parts.path == "/data/playlist.csv"
        and port in (None, 80)
        and not parts.username
        and not parts.password
        and not parts.query
        and not parts.fragment
    )


def parse_station_playlist(content: bytes) -> list[tuple[int, str]]:
    """Match Edge's parseCSV/indexPlaylist row filtering and 1-based IDs."""
    stations: list[tuple[int, str]] = []
    for line in content.decode("utf-8-sig", errors="replace").split("\n"):
        # Edge requires two TAB separators and a nonempty remainder after the
        # second one; the fourth column is optional. Invalid lines get no ID.
        columns = line.split("\t", 2)
        if len(columns) != 3 or not columns[0] or not columns[1] or not columns[2]:
            continue
        station_id = len(stations) + 1
        title = columns[0].strip() or f"Station {station_id}"
        stations.append((station_id, title))
        if len(stations) >= MAX_BROWSE_STATIONS:
            break
    return stations
