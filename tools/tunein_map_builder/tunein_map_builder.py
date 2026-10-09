#!/usr/bin/env python3
"""Build a three-column FusionEdge station map from a playlist.

Output format (UTF-8, TAB separated):

    station name<TAB>remote image URL<TAB>local image filename

The program accepts the normal TAB separated FusionEdge playlist as well as
semicolon/comma separated spreadsheet exports. Pillow decodes and converts
downloaded logos. With no command-line arguments it starts a small desktop GUI.
"""

from __future__ import annotations

import argparse
import csv
import html
import io
import json
import os
import queue
import re
import shutil
import ssl
import struct
import sys
import tempfile
import threading
import time
import unicodedata
import urllib.parse
import urllib.request
import uuid
import webbrowser
import zlib
from dataclasses import asdict, dataclass, field, replace
from difflib import SequenceMatcher
from pathlib import Path
from typing import Iterable, Sequence

try:
    from PIL import Image, ImageOps, UnidentifiedImageError
    import truststore
except ImportError as exc:
    raise SystemExit(
        "Pillow and truststore are required for image conversion and HTTPS. "
        "Use run_windows.bat or install tools/tunein_map_builder/requirements.txt."
    ) from exc


APP_NAME = "FusionEdge TuneIn map builder"
SEARCH_ENDPOINT = "https://opml.radiotime.com/Search.ashx"
DESCRIBE_ENDPOINT = "https://opml.radiotime.com/Describe.ashx"
SITEMAP_INDEX = "https://tunein.com/sitemap_index.xml"
USER_AGENT = "FusionEdge-TuneIn-Map-Builder/1.0"
IMAGE_SUFFIXES = (".png", ".jpg", ".jpeg", ".webp")
SEARCH_DELAY_SECONDS = 0.45
MAX_IMAGE_BYTES = 2 * 1024 * 1024
MAX_IMAGE_PIXELS = 12_000_000
DEFAULT_LOCAL_LOGO_SIZE = 120
MAX_INITIAL_FILL_RATIO = 0.80
FALLBACK_NAMES = ("plmodeweb.png", "plmodesd.png", "plmodedlna.png", "plmodebt.png")
FALLBACK_DIR = Path(__file__).resolve().parent / "plmode_icons"
PARTITIONS_CSV = Path(__file__).resolve().parents[2] / "partitions.csv"
TLS_CONTEXT = truststore.SSLContext(ssl.PROTOCOL_TLS_CLIENT)


@dataclass
class SearchCandidate:
    name: str
    image_url: str
    guide_id: str = ""
    subtext: str = ""
    score: float = 0.0


@dataclass
class MapEntry:
    station: str
    local_file: str = ""
    remote_url: str = ""
    status: str = ""
    stream_url: str = ""
    local_size: int = 0
    candidates: list[SearchCandidate] = field(default_factory=list)
    map_remote_explicit: bool = False
    include_logo: bool = True


@dataclass(frozen=True)
class ExportPlan:
    data_dir: Path
    image_sources: dict[str, Path]
    map_entries: list[MapEntry]
    nonstation_bytes: int
    stations_bytes: int
    total_bytes: int
    limit_bytes: int


@dataclass(frozen=True)
class DownloadedLogo:
    data: bytes
    extension: str
    source_url: str
    width: int = 0
    height: int = 0


@dataclass
class LoadReport:
    delimiter: str
    encoding: str
    total_rows: int = 0
    skipped_rows: int = 0
    duplicate_rows: int = 0
    replacement_character_rows: list[int] = field(default_factory=list)

    @property
    def delimiter_name(self) -> str:
        return {"\t": "TAB", ";": "semicolon", ",": "comma"}.get(
            self.delimiter, repr(self.delimiter)
        )


@dataclass(frozen=True)
class CatalogStation:
    name: str
    guide_id: str
    normalized_name: str


_catalog_lock = threading.Lock()
_catalog: list[CatalogStation] | None = None
_catalog_token_index: dict[str, list[int]] | None = None


def _read_text(path: Path) -> tuple[str, str]:
    raw = path.read_bytes()
    attempts = (
        ("utf-8-sig", "UTF-8"),
        ("cp1250", "Windows-1250"),
        ("cp1252", "Windows-1252"),
    )
    for codec, label in attempts:
        try:
            return raw.decode(codec), label
        except UnicodeDecodeError:
            pass
    return raw.decode("utf-8", errors="replace"), "UTF-8 with replacement"


def detect_delimiter(text: str) -> str:
    sample_lines = [
        line for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")
    ][:20]
    if not sample_lines:
        return "\t"
    sample = "\n".join(sample_lines)
    try:
        return csv.Sniffer().sniff(sample, delimiters="\t;,").delimiter
    except csv.Error:
        counts = {separator: sum(line.count(separator) for line in sample_lines) for separator in "\t;,"}
        return max(counts, key=counts.get)


def clean_station_name(value: str) -> str:
    value = value.strip().lstrip("\ufeff")
    # FusionEdge treats a leading dot in playlist.csv as a control marker and
    # does not include it in the station name used for map.csv lookup.
    if value.startswith("."):
        value = value[1:].lstrip()
    return value


def station_key(value: str) -> str:
    return " ".join(clean_station_name(value).casefold().split())


def build_search_query(station: str) -> str:
    """Remove FusionEdge-only labels that are not part of a station name."""
    value = clean_station_name(station)
    value = re.sub(r"(?i)^teszt\s+", "", value).strip()
    value = re.sub(
        r"(?i)\s*\([^)]*(?:hi[ -]?fi|vor|flac|opus|aac|mp3|\d+\s*k(?:bps)?)[^)]*\)\s*",
        " ",
        value,
    )
    value = re.sub(r"(?i)^radioparadise\b", "Radio Paradise", value)
    value = re.sub(r"(?i)(?<=[A-Za-z])fm$", " FM", value)
    return " ".join(value.split())


def _looks_like_header(row: Sequence[str]) -> bool:
    if not row:
        return False
    first = station_key(row[0])
    second = row[1].casefold() if len(row) > 1 else ""
    return first in {"station", "station name", "name", "állomás", "állomás neve"} and (
        "url" in second or "stream" in second
    )


def _looks_like_image_url(value: str) -> bool:
    value = value.strip()
    if not value.lower().startswith(("http://", "https://")):
        return False
    parsed = urllib.parse.urlsplit(value)
    path = parsed.path.casefold()
    host = parsed.netloc.casefold()
    return path.endswith(IMAGE_SUFFIXES) or (
        "tunein.com" in host and any(token in path for token in ("logo", "image", "/s"))
    )


def normalize_tunein_image_url(value: str) -> str:
    """Keep the actual artwork path; only use HTTP for known TuneIn CDNs."""
    value = value.strip()
    if not value:
        return ""
    parsed = urllib.parse.urlsplit(value)
    if "tunein.com" not in parsed.netloc.casefold():
        return value

    # Artwork URLs are PC-side source information. Rewriting a JPEG suffix to
    # PNG can return JSON or an unrelated CDN fallback instead of an image.
    return urllib.parse.urlunsplit(
        ("http", parsed.netloc, parsed.path, parsed.query, parsed.fragment)
    )


def validate_manual_image_url(value: str) -> str:
    """Validate and normalize an image URL entered by the user.

    The downloader verifies image bytes, so no particular suffix is required.
    """
    value = normalize_tunein_image_url(value)
    if not value:
        raise ValueError("Enter an image URL first.")
    parsed = urllib.parse.urlsplit(value)
    if parsed.scheme.casefold() not in {"http", "https"} or not parsed.netloc:
        raise ValueError("The image URL must start with http:// or https://.")
    return value


def _tunein_station_id(url: str) -> str:
    parsed = urllib.parse.urlsplit(url)
    if "tunein.com" not in parsed.netloc.casefold():
        return ""
    match = re.search(r"(?i)(?:^|/)s(\d+)(?:d|q|g|t)?(?:[./]|$)", parsed.path)
    return match.group(1) if match else ""


def download_candidate_urls(url: str) -> list[str]:
    """Try the selected URL before optional TuneIn station-id fallbacks."""
    original = normalize_tunein_image_url(url)
    station_id = _tunein_station_id(original)
    alternatives: list[str] = []
    if station_id:
        alternatives = [
            f"http://cdn-radiotime-logos.tunein.com/s{station_id}d.png",
            f"http://cdn-profiles.tunein.com/s{station_id}/images/logod.png",
            f"http://cdn-profiles.tunein.com/s{station_id}/images/logod.jpg",
        ]
    ordered = [original] + alternatives
    result: list[str] = []
    seen: set[str] = set()
    for candidate in ordered:
        key = candidate.casefold()
        if candidate and key not in seen:
            seen.add(key)
            result.append(candidate)
    return result


def image_info(data: bytes) -> tuple[str, int, int, bool]:
    """Return (extension, width, height, decoder-compatible)."""
    if len(data) >= 24 and data[:8] == b"\x89PNG\r\n\x1a\n":
        width = int.from_bytes(data[16:20], "big")
        height = int.from_bytes(data[20:24], "big")
        return ".png", width, height, width > 0 and height > 0

    if len(data) < 4 or data[:2] != b"\xff\xd8":
        return "", 0, 0, False
    offset = 2
    while offset + 3 < len(data):
        if data[offset] != 0xFF:
            offset += 1
            continue
        while offset < len(data) and data[offset] == 0xFF:
            offset += 1
        if offset >= len(data):
            break
        marker = data[offset]
        offset += 1
        if marker in (0xD9, 0xDA):
            break
        if marker == 0x01 or 0xD0 <= marker <= 0xD8:
            continue
        if offset + 1 >= len(data):
            break
        segment_length = int.from_bytes(data[offset : offset + 2], "big")
        if segment_length < 2 or offset + segment_length > len(data):
            break
        is_sof = (
            0xC0 <= marker <= 0xC3
            or 0xC5 <= marker <= 0xC7
            or 0xC9 <= marker <= 0xCB
            or 0xCD <= marker <= 0xCF
        )
        if is_sof and segment_length >= 7:
            height = int.from_bytes(data[offset + 3 : offset + 5], "big")
            width = int.from_bytes(data[offset + 5 : offset + 7], "big")
            return ".jpg", width, height, marker == 0xC0 and width > 0 and height > 0
        offset += segment_length
    return ".jpg", 0, 0, False


def _paeth_predictor(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def decode_png_rgba(data: bytes) -> tuple[int, int, bytearray]:
    """Decode the 8-bit, non-interlaced PNG formats used by TuneIn."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG image")
    width = height = bit_depth = color_type = interlace = 0
    palette = b""
    transparency = b""
    compressed = bytearray()
    offset = 8
    while offset + 12 <= len(data):
        length = int.from_bytes(data[offset : offset + 4], "big")
        chunk_type = data[offset + 4 : offset + 8]
        chunk_start = offset + 8
        chunk_end = chunk_start + length
        if chunk_end + 4 > len(data):
            raise ValueError("truncated PNG chunk")
        payload = data[chunk_start:chunk_end]
        if chunk_type == b"IHDR":
            if length != 13:
                raise ValueError("invalid PNG header")
            width, height, bit_depth, color_type, compression, filtering, interlace = (
                struct.unpack(">IIBBBBB", payload)
            )
            if compression != 0 or filtering != 0:
                raise ValueError("unsupported PNG compression/filter method")
        elif chunk_type == b"PLTE":
            palette = payload
        elif chunk_type == b"tRNS":
            transparency = payload
        elif chunk_type == b"IDAT":
            compressed.extend(payload)
        elif chunk_type == b"IEND":
            break
        offset = chunk_end + 4

    if width <= 0 or height <= 0 or bit_depth != 8 or interlace != 0:
        raise ValueError("only 8-bit, non-interlaced PNG images can be resized")
    bytes_per_pixel = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if bytes_per_pixel is None:
        raise ValueError(f"unsupported PNG color type {color_type}")
    try:
        scanlines = zlib.decompress(bytes(compressed))
    except zlib.error as exc:
        raise ValueError(f"invalid PNG data: {exc}") from exc
    stride = width * bytes_per_pixel
    expected = height * (stride + 1)
    if len(scanlines) != expected:
        raise ValueError("unsupported or truncated PNG scanlines")

    rows: list[bytearray] = []
    source_offset = 0
    previous = bytearray(stride)
    for _row_index in range(height):
        filter_type = scanlines[source_offset]
        source_offset += 1
        encoded = scanlines[source_offset : source_offset + stride]
        source_offset += stride
        row = bytearray(stride)
        for index, value in enumerate(encoded):
            left = row[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
            above = previous[index]
            upper_left = previous[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            elif filter_type == 4:
                predictor = _paeth_predictor(left, above, upper_left)
            else:
                raise ValueError(f"unsupported PNG row filter {filter_type}")
            row[index] = (value + predictor) & 0xFF
        rows.append(row)
        previous = row

    rgba = bytearray(width * height * 4)
    target = 0
    for row in rows:
        if color_type == 6:
            rgba[target : target + width * 4] = row
            target += width * 4
        elif color_type == 2:
            for source in range(0, len(row), 3):
                rgba[target : target + 4] = row[source : source + 3] + b"\xff"
                target += 4
        elif color_type == 0:
            for gray in row:
                rgba[target : target + 4] = bytes((gray, gray, gray, 255))
                target += 4
        elif color_type == 4:
            for source in range(0, len(row), 2):
                gray, alpha = row[source : source + 2]
                rgba[target : target + 4] = bytes((gray, gray, gray, alpha))
                target += 4
        else:  # indexed color, color type 3
            for palette_index in row:
                palette_offset = palette_index * 3
                if palette_offset + 2 >= len(palette):
                    raise ValueError("PNG palette index is out of range")
                alpha = transparency[palette_index] if palette_index < len(transparency) else 255
                rgba[target : target + 4] = palette[
                    palette_offset : palette_offset + 3
                ] + bytes((alpha,))
                target += 4
    return width, height, rgba


def _axis_contributions(source_size: int, target_size: int) -> list[list[tuple[int, float]]]:
    scale = source_size / target_size
    contributions: list[list[tuple[int, float]]] = []
    for target_index in range(target_size):
        start = target_index * scale
        end = (target_index + 1) * scale
        first = int(start)
        last = min(source_size, int(end) + (0 if end.is_integer() else 1))
        weights: list[tuple[int, float]] = []
        for source_index in range(first, last):
            overlap = min(end, source_index + 1) - max(start, source_index)
            if overlap > 0:
                weights.append((source_index, overlap))
        contributions.append(weights)
    return contributions


def resize_rgba_fit(
    rgba: bytearray, width: int, height: int, target_size: int
) -> bytearray:
    """Area-resample into a square transparent canvas without stretching."""
    if width <= 0 or height <= 0 or target_size <= 0:
        raise ValueError("invalid image dimensions")
    scale = min(target_size / width, target_size / height)
    scaled_width = max(1, min(target_size, round(width * scale)))
    scaled_height = max(1, min(target_size, round(height * scale)))
    x_weights = _axis_contributions(width, scaled_width)
    y_weights = _axis_contributions(height, scaled_height)
    canvas = bytearray(target_size * target_size * 4)
    x_offset = (target_size - scaled_width) // 2
    y_offset = (target_size - scaled_height) // 2

    for target_y, source_rows in enumerate(y_weights):
        for target_x, source_columns in enumerate(x_weights):
            alpha_sum = red_sum = green_sum = blue_sum = total_weight = 0.0
            for source_y, y_weight in source_rows:
                for source_x, x_weight in source_columns:
                    weight = x_weight * y_weight
                    source = (source_y * width + source_x) * 4
                    alpha = rgba[source + 3]
                    total_weight += weight
                    alpha_sum += alpha * weight
                    red_sum += rgba[source] * alpha * weight
                    green_sum += rgba[source + 1] * alpha * weight
                    blue_sum += rgba[source + 2] * alpha * weight
            target = (
                (target_y + y_offset) * target_size + target_x + x_offset
            ) * 4
            alpha = round(alpha_sum / total_weight) if total_weight else 0
            canvas[target + 3] = max(0, min(255, alpha))
            if alpha_sum > 0:
                canvas[target] = max(0, min(255, round(red_sum / alpha_sum)))
                canvas[target + 1] = max(0, min(255, round(green_sum / alpha_sum)))
                canvas[target + 2] = max(0, min(255, round(blue_sum / alpha_sum)))
    return canvas


def _filtered_png_row(row: bytes, previous: bytes, filter_type: int, bpp: int = 4) -> bytes:
    result = bytearray(len(row))
    for index, value in enumerate(row):
        left = row[index - bpp] if index >= bpp else 0
        above = previous[index]
        upper_left = previous[index - bpp] if index >= bpp else 0
        if filter_type == 0:
            predictor = 0
        elif filter_type == 1:
            predictor = left
        elif filter_type == 2:
            predictor = above
        elif filter_type == 3:
            predictor = (left + above) // 2
        else:
            predictor = _paeth_predictor(left, above, upper_left)
        result[index] = (value - predictor) & 0xFF
    return bytes(result)


def _png_chunk(chunk_type: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + chunk_type
        + payload
        + struct.pack(">I", zlib.crc32(chunk_type + payload) & 0xFFFFFFFF)
    )


def encode_png_rgba(width: int, height: int, rgba: bytes) -> bytes:
    stride = width * 4
    if len(rgba) != stride * height:
        raise ValueError("RGBA buffer size does not match its dimensions")
    filtered = bytearray()
    previous = bytes(stride)
    for row_index in range(height):
        row = bytes(rgba[row_index * stride : (row_index + 1) * stride])
        candidates = [_filtered_png_row(row, previous, filter_type) for filter_type in range(5)]
        scores = [
            sum(value if value < 128 else 256 - value for value in candidate)
            for candidate in candidates
        ]
        selected = min(range(5), key=scores.__getitem__)
        filtered.append(selected)
        filtered.extend(candidates[selected])
        previous = row
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", header)
        + _png_chunk(b"IDAT", zlib.compress(bytes(filtered), level=9))
        + _png_chunk(b"IEND", b"")
    )


def resize_png(data: bytes, target_size: int = DEFAULT_LOCAL_LOGO_SIZE) -> bytes:
    width, height, rgba = decode_png_rgba(data)
    resized = resize_rgba_fit(rgba, width, height, target_size)
    return encode_png_rgba(target_size, target_size, resized)


def resize_downloaded_logo(
    logo: DownloadedLogo, target_size: int = DEFAULT_LOCAL_LOGO_SIZE
) -> DownloadedLogo:
    return convert_image_to_png(logo.data, logo.source_url, target_size)


def convert_image_to_png(
    data: bytes, source_url: str, target_size: int = 0
) -> DownloadedLogo:
    """Fully decode PNG/JPEG/WebP input and emit a real, ordinary RGBA PNG."""
    try:
        with Image.open(io.BytesIO(data)) as source:
            if source.format not in {"PNG", "JPEG", "WEBP"}:
                raise ValueError(f"unsupported image format: {source.format}")
            width, height = source.size
            if width <= 0 or height <= 0 or width * height > MAX_IMAGE_PIXELS:
                raise ValueError("image dimensions exceed the safe limit")
            source.load()  # A .png suffix or a valid header alone is not enough.
            image = ImageOps.exif_transpose(source).convert("RGBA")
        if target_size:
            fitted = ImageOps.contain(
                image, (target_size, target_size), method=Image.Resampling.LANCZOS
            )
            canvas = Image.new("RGBA", (target_size, target_size), (0, 0, 0, 0))
            canvas.alpha_composite(
                fitted,
                ((target_size - fitted.width) // 2, (target_size - fitted.height) // 2),
            )
            image = canvas
        result = io.BytesIO()
        image.save(result, format="PNG", optimize=True)
        return DownloadedLogo(result.getvalue(), ".png", source_url, image.width, image.height)
    except (UnidentifiedImageError, OSError) as exc:
        raise ValueError("response is not a valid PNG/JPEG/WebP image") from exc


def _request_image_bytes(url: str, timeout: float) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=timeout, context=TLS_CONTEXT) as response:
        content_length = response.headers.get("Content-Length")
        if content_length and int(content_length) > MAX_IMAGE_BYTES:
            raise ValueError(f"image exceeds {MAX_IMAGE_BYTES // 1024} KiB")
        data = response.read(MAX_IMAGE_BYTES + 1)
    if len(data) > MAX_IMAGE_BYTES:
        raise ValueError(f"image exceeds {MAX_IMAGE_BYTES // 1024} KiB")
    return data


def download_logo(url: str, timeout: float = 20.0) -> DownloadedLogo:
    errors: list[str] = []
    for candidate in download_candidate_urls(url):
        try:
            data = _request_image_bytes(candidate, timeout)
            return convert_image_to_png(data, candidate)
        except Exception as exc:
            errors.append(f"{candidate}: {exc}")
    raise RuntimeError("; ".join(errors) if errors else "no image URL")


def local_logo_stem(station: str) -> str:
    value = unicodedata.normalize("NFKD", clean_station_name(station))
    value = value.encode("ascii", errors="ignore").decode("ascii").casefold()
    value = re.sub(r"[^a-z0-9]+", "-", value).strip("-")
    return (value[:56].rstrip("-") or "station")


def unique_logo_filename(
    station: str, extension: str, requested: str, used: set[str]
) -> str:
    requested_name = Path(requested).name if requested else ""
    base = Path(requested_name).stem if requested_name else local_logo_stem(station)
    base = re.sub(r"[^A-Za-z0-9._-]+", "-", base).strip(".-") or "station"
    candidate = f"{base}{extension}"
    serial = 2
    while candidate.casefold() in used:
        candidate = f"{base}-{serial}{extension}"
        serial += 1
    used.add(candidate.casefold())
    return candidate


def format_size(byte_count: int) -> str:
    return f"{byte_count / 1024.0:.1f} KiB"


def find_image_url(row: Sequence[str]) -> str:
    # Spreadsheet exports used by the project have the logo in column five;
    # still scan every non-name field so custom playlists work as well.
    values = list(row[4:5]) + list(row[1:4]) + list(row[5:])
    for value in values:
        if _looks_like_image_url(value):
            return normalize_tunein_image_url(value)
    return ""


def load_playlist(path: Path) -> tuple[list[MapEntry], LoadReport]:
    text, encoding = _read_text(path)
    delimiter = detect_delimiter(text)
    report = LoadReport(delimiter=delimiter, encoding=encoding)
    entries: list[MapEntry] = []
    seen: set[str] = set()
    reader = csv.reader(io.StringIO(text), delimiter=delimiter)

    for row_number, row in enumerate(reader, start=1):
        if not row or not any(cell.strip() for cell in row):
            continue
        if row[0].lstrip().startswith("#") or _looks_like_header(row):
            report.skipped_rows += 1
            continue
        report.total_rows += 1
        station = clean_station_name(row[0])
        if not station:
            report.skipped_rows += 1
            continue
        if any("\ufffd" in cell for cell in row):
            report.replacement_character_rows.append(row_number)
        key = station_key(station)
        if key in seen:
            report.duplicate_rows += 1
            continue
        seen.add(key)
        remote = find_image_url(row)
        status = "playlist URL" if remote else "missing URL"
        stream_url = row[1].strip() if len(row) > 1 else ""
        entries.append(
            MapEntry(station=station, remote_url=remote, status=status, stream_url=stream_url)
        )
    return entries, report


def load_existing_map(path: Path) -> dict[str, MapEntry]:
    text, _encoding = _read_text(path)
    delimiter = detect_delimiter(text)
    result: dict[str, MapEntry] = {}
    for row in csv.reader(io.StringIO(text), delimiter=delimiter):
        if not row or not any(cell.strip() for cell in row) or row[0].lstrip().startswith("#"):
            continue
        station = clean_station_name(row[0])
        if not station:
            continue
        second = row[1].strip() if len(row) > 1 else ""
        third = row[2].strip() if len(row) > 2 else ""
        local_file = ""
        remote_url = ""
        if len(row) >= 3:
            if _looks_like_image_url(second):
                # New format: station, remote URL, optional local override.
                remote_url = normalize_tunein_image_url(second)
                local_file = third
            elif _looks_like_image_url(third):
                # Accept the first experimental format as well: station,
                # local file, remote URL.
                local_file = second
                remote_url = normalize_tunein_image_url(third)
            else:
                # New format with an empty/unresolved remote URL.
                remote_url = second
                local_file = third
        else:
            # Legacy FusionEdge format: station, local file.
            local_file = second
        result[station_key(station)] = MapEntry(
            station,
            local_file,
            remote_url,
            "existing map",
            map_remote_explicit=len(row) >= 3,
        )
    return result


def merge_existing_map(entries: Iterable[MapEntry], existing: dict[str, MapEntry]) -> int:
    matches = 0
    for entry in entries:
        previous = existing.get(station_key(entry.station))
        if previous is None:
            continue
        matches += 1
        entry.local_file = previous.local_file
        if previous.map_remote_explicit:
            entry.remote_url = previous.remote_url
        elif not entry.remote_url and previous.remote_url:
            entry.remote_url = previous.remote_url
        entry.status = "playlist + map" if entry.remote_url else "local map only"
    return matches


def map_csv_bytes(entries: Iterable[MapEntry], selected_only: bool = False) -> bytes:
    rows = []
    for entry in entries:
        local_file = entry.local_file.strip() if (entry.include_logo or not selected_only) else ""
        if local_file and Path(local_file).suffix.casefold() != ".png":
            raise ValueError(
                f"{entry.station}: the LittleFS logo filename must end in .png"
            )
        rows.append(
            (
                clean_station_name(entry.station),
                normalize_tunein_image_url(entry.remote_url),
                local_file,
            )
        )
    buffer = io.StringIO(newline="")
    writer = csv.writer(buffer, delimiter="\t", lineterminator="\n")
    writer.writerows(rows)
    return buffer.getvalue().encode("utf-8")


def write_map(path: Path, entries: Iterable[MapEntry], selected_only: bool = False) -> None:
    content = map_csv_bytes(entries, selected_only)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_bytes(content)
    temporary.replace(path)


def validate_data_dir(data_dir: Path) -> Path:
    data_dir = data_dir.resolve()
    if data_dir.name.casefold() != "data" or not data_dir.is_dir():
        raise ValueError("Select the project's data folder (the folder named 'data').")
    if not (data_dir / "images").is_dir():
        raise ValueError("The selected data folder has no images subfolder.")
    return data_dir


def nonstation_data_size(data_dir: Path) -> int:
    """Count LittleFS input files while excluding the old station artwork tree."""
    total = 0
    excluded = data_dir / "images" / "stations"
    for root, dirs, files in os.walk(data_dir):
        root_path = Path(root)
        dirs[:] = [
            name for name in dirs
            if root_path / name != excluded and not (root_path / name).is_symlink()
        ]
        for name in files:
            path = root_path / name
            if path.is_symlink():
                raise ValueError(f"Symbolic link in data folder: {path}")
            total += path.stat().st_size
    return total


def littlefs_partition_bytes(partitions_csv: Path = PARTITIONS_CSV) -> int:
    with partitions_csv.open("r", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if len(row) < 5 or row[0].lstrip().startswith("#"):
                continue
            if row[1].strip().casefold() == "data" and row[2].strip().casefold() in ("spiffs", "littlefs"):
                return int(row[4].strip(), 0)
    raise ValueError(f"LittleFS partition was not found in {partitions_csv}")


def _safe_logo_name(name: str) -> str:
    if not name or Path(name).name != name or name in (".", "..") or "\\" in name:
        raise ValueError(f"Invalid local logo filename: {name!r}")
    if Path(name).suffix.casefold() != ".png":
        raise ValueError(f"Local logo must be PNG: {name}")
    return name


def build_export_plan(
    data_dir: Path,
    entries: Sequence[MapEntry],
    staging_dir: Path | None,
    fallback_dir: Path = FALLBACK_DIR,
) -> ExportPlan:
    data_dir = validate_data_dir(data_dir)
    old_stations = data_dir / "images" / "stations"
    sources: dict[str, Path] = {}
    names: dict[str, str] = {}
    fallback_keys = {name.casefold() for name in FALLBACK_NAMES}
    for name in FALLBACK_NAMES:
        source = fallback_dir / name
        if not source.is_file():
            raise ValueError(f"Mandatory fallback icon missing: {source}")
        sources[name] = source
        names[name.casefold()] = name
    for entry in entries:
        if not entry.include_logo or not entry.local_file.strip():
            continue
        name = _safe_logo_name(entry.local_file.strip())
        key = name.casefold()
        if key in names and names[key] != name:
            raise ValueError(f"Case-insensitive logo filename collision: {name}")
        if key in fallback_keys:
            raise ValueError(f"Station logo conflicts with mandatory fallback icon: {name}")
        names[key] = name
        source = staging_dir / name if staging_dir else None
        if not source or not source.is_file():
            source = old_stations / name
        if not source.is_file():
            raise ValueError(f"Selected logo is not downloaded: {entry.station} ({name})")
        sources[name] = source
    for source in sources.values():
        with source.open("rb") as handle:
            if handle.read(8) != b"\x89PNG\r\n\x1a\n":
                raise ValueError(f"Not a PNG file: {source}")
    exported_entries = [replace(entry, local_file="" if not entry.include_logo else entry.local_file) for entry in entries]
    station_size = sum(path.stat().st_size for path in sources.values()) + len(map_csv_bytes(exported_entries))
    other_size = nonstation_data_size(data_dir)
    limit = int(littlefs_partition_bytes() * MAX_INITIAL_FILL_RATIO)
    return ExportPlan(data_dir, sources, exported_entries, other_size, station_size,
                      other_size + station_size, limit)


def export_to_data(plan: ExportPlan, backup_dir: Path) -> Path | None:
    """Install a clean stations folder; keep the old one outside data, rollback on error."""
    if plan.total_bytes > plan.limit_bytes:
        raise ValueError("Selected logos exceed the 80% LittleFS image budget.")
    data_dir = validate_data_dir(plan.data_dir)
    destination = data_dir / "images" / "stations"
    backup_dir = backup_dir.resolve()
    if backup_dir == data_dir or data_dir in backup_dir.parents:
        raise ValueError("Backup folder must be outside the project's data folder.")
    backup_dir.mkdir(parents=True, exist_ok=True)
    build = Path(tempfile.mkdtemp(prefix="stations-export-", dir=backup_dir))
    for name, source in plan.image_sources.items():
        shutil.copy2(source, build / name)
    write_map(build / "map.csv", plan.map_entries)
    previous: Path | None = None
    if destination.exists():
        previous = backup_dir / f"stations-before-{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:8]}"
        destination.rename(previous)
    try:
        build.rename(destination)
    except OSError:
        if previous is not None:
            previous.rename(destination)
        raise
    return previous


def write_project(
    path: Path,
    entries: Sequence[MapEntry],
    playlist_path: Path | None,
    resize_to_120: bool,
    staging_dir: Path | None = None,
) -> None:
    """Save the full editing session, independent of the firmware map.csv."""
    document = {
        "format": "fusionedge-tunein-map-project",
        "version": 2,
        "playlist_path": str(playlist_path) if playlist_path else "",
        "resize_to_120": resize_to_120,
        "staging_dir": str(staging_dir) if staging_dir else "",
        "entries": [asdict(entry) for entry in entries],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def load_project(path: Path) -> tuple[list[MapEntry], Path | None, bool, Path | None]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or (
        document.get("format") != "fusionedge-tunein-map-project"
        or document.get("version") not in (1, 2)
    ):
        raise ValueError("Not a supported TuneIn map project file.")
    rows = document.get("entries")
    playlist_path = document.get("playlist_path")
    resize_to_120 = document.get("resize_to_120")
    staging_dir = document.get("staging_dir", "")
    if (
        not isinstance(rows, list)
        or not isinstance(playlist_path, str)
        or not isinstance(resize_to_120, bool)
        or not isinstance(staging_dir, str)
    ):
        raise ValueError("The project file has invalid settings or entries.")
    entries: list[MapEntry] = []
    seen: set[str] = set()
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("candidates"), list):
            raise ValueError("The project file contains an invalid station.")
        try:
            candidates = [SearchCandidate(**candidate) for candidate in row["candidates"]]
            entry = MapEntry(**{**row, "candidates": candidates})
        except (TypeError, ValueError) as exc:
            raise ValueError("The project file contains an invalid station.") from exc
        if not isinstance(entry.station, str) or not entry.station.strip():
            raise ValueError("The project file contains an unnamed station.")
        key = station_key(entry.station)
        if key in seen:
            raise ValueError(f"Duplicate station in project: {entry.station}")
        seen.add(key)
        entries.append(entry)
    return (entries, Path(playlist_path) if playlist_path else None, resize_to_120,
            Path(staging_dir) if staging_dir else None)


def mark_download_failed(entry: MapEntry, error: str) -> None:
    """Never export a failed row with a stale local image reference."""
    entry.local_file = ""
    entry.local_size = 0
    entry.status = f"download failed: {error}"


def clear_logo(entry: MapEntry) -> None:
    """Remove both map references; an old image file may safely remain on disk."""
    entry.remote_url = ""
    entry.local_file = ""
    entry.local_size = 0
    entry.include_logo = False
    entry.status = "no logo (manual)"


def select_image_url(entry: MapEntry, url: str) -> None:
    """A new source must not silently reuse the previous downloaded logo."""
    if entry.remote_url != url:
        entry.local_file = ""
        entry.local_size = 0
    entry.remote_url = url
    if url:
        entry.include_logo = True


def _normalized_match_text(value: str) -> str:
    value = unicodedata.normalize("NFKD", value)
    value = "".join(char for char in value if not unicodedata.combining(char))
    value = re.sub(r"(?i)(?<=[a-z0-9])fm\b", " FM", value)
    value = value.casefold()
    value = re.sub(r"\([^)]*(?:hi[ -]?fi|kbps|aac|mp3)[^)]*\)", " ", value)
    value = re.sub(r"\b(?:online|live|hifi|hi fi)\b", " ", value)
    return " ".join(re.findall(r"[a-z0-9]+", value))


def _candidate_score(query: str, candidate_name: str) -> float:
    left = _normalized_match_text(query)
    right = _normalized_match_text(candidate_name)
    if not left or not right:
        return 0.0
    if left == right:
        return 1.0
    ratio = SequenceMatcher(None, left, right).ratio()
    left_tokens = set(left.split())
    right_tokens = set(right.split())
    overlap = len(left_tokens & right_tokens)
    if overlap:
        precision = overlap / len(right_tokens)
        recall = overlap / len(left_tokens)
        token_f1 = 2.0 * precision * recall / (precision + recall)
        # Token overlap is useful, but word order still distinguishes names
        # such as "Radio Paradise" from "Paradise Radio".
        ratio = max(ratio, token_f1 * 0.88)
    if left in right or right in left:
        length_ratio = min(len(left), len(right)) / max(len(left), len(right))
        ratio = max(ratio, 0.80 + 0.19 * length_ratio)
    return ratio


def _query_token_recall(query: str, candidate_name: str) -> float:
    stopwords = {"a", "an", "and", "of", "the"}
    query_tokens = set(_normalized_match_text(query).split()) - stopwords
    candidate_tokens = set(_normalized_match_text(candidate_name).split()) - stopwords
    if not query_tokens:
        return 0.0
    return len(query_tokens & candidate_tokens) / len(query_tokens)


def _request_bytes(url: str, timeout: float) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=timeout, context=TLS_CONTEXT) as response:
        return response.read()


def _load_tunein_catalog(timeout: float = 25.0) -> tuple[list[CatalogStation], dict[str, list[int]]]:
    global _catalog, _catalog_token_index
    if _catalog is not None and _catalog_token_index is not None:
        return _catalog, _catalog_token_index
    with _catalog_lock:
        if _catalog is not None and _catalog_token_index is not None:
            return _catalog, _catalog_token_index

        index_text = _request_bytes(SITEMAP_INDEX, timeout).decode("utf-8", errors="replace")
        sitemap_urls = re.findall(r"<loc>([^<]*stations\d+\.xml)</loc>", index_text)
        catalog: list[CatalogStation] = []
        token_index: dict[str, list[int]] = {}
        seen_ids: set[str] = set()
        for sitemap_url in sitemap_urls:
            sitemap_text = _request_bytes(html.unescape(sitemap_url), timeout).decode(
                "utf-8", errors="replace"
            )
            for station_url in re.findall(r"<loc>([^<]+)</loc>", sitemap_text):
                station_url = html.unescape(station_url)
                match = re.search(r"/radio/(.+)-(s\d+)/?$", station_url)
                if not match:
                    continue
                guide_id = match.group(2)
                if guide_id in seen_ids:
                    continue
                seen_ids.add(guide_id)
                name = urllib.parse.unquote(match.group(1)).replace("-", " ")
                name = " ".join(name.split())
                normalized = _normalized_match_text(name)
                if not normalized:
                    continue
                station_index = len(catalog)
                catalog.append(CatalogStation(name, guide_id, normalized))
                for token in set(normalized.split()):
                    token_index.setdefault(token, []).append(station_index)
        _catalog = catalog
        _catalog_token_index = token_index
        return catalog, token_index


def _catalog_matches(query: str, limit: int = 8) -> list[tuple[CatalogStation, float]]:
    catalog, token_index = _load_tunein_catalog()
    normalized = _normalized_match_text(query)
    tokens = set(normalized.split())
    pools = [token_index[token] for token in tokens if token in token_index]
    if pools:
        # A station may abbreviate one word (Budapest -> BDPST), so restricting
        # the pool to the rarest exact token would hide an otherwise strong
        # match. The union is still small enough for the public catalog.
        candidate_indexes = set().union(*pools)
    else:
        candidate_indexes = range(len(catalog))
    scored = [
        (catalog[index], _candidate_score(query, catalog[index].name))
        for index in candidate_indexes
    ]
    scored.sort(
        key=lambda item: (_query_token_recall(query, item[0].name), item[1]),
        reverse=True,
    )
    return scored[:limit]


def _describe_station(
    guide_id: str, query: str, fallback_name: str, timeout: float
) -> SearchCandidate | None:
    params = urllib.parse.urlencode({"id": guide_id, "render": "json"})
    payload = json.loads(_request_bytes(f"{DESCRIBE_ENDPOINT}?{params}", timeout))
    body = payload.get("body", []) if isinstance(payload, dict) else []
    if not body or not isinstance(body[0], dict):
        return None
    item = body[0]
    image = normalize_tunein_image_url(str(item.get("logo", "")))
    if not image:
        return None
    name = str(item.get("name") or fallback_name).strip()
    return SearchCandidate(
        name=name,
        image_url=image,
        guide_id=guide_id,
        subtext=str(item.get("slogan", "") or "").strip(),
        score=_candidate_score(query, name),
    )


def search_tunein(station: str, timeout: float = 12.0) -> list[SearchCandidate]:
    query = build_search_query(station)
    candidates_by_id: dict[str, SearchCandidate] = {}

    # The legacy search is useful for live metadata but can omit valid stations
    # and mixes several content types. Keep only live station records.
    try:
        params = urllib.parse.urlencode({"query": query, "render": "json"})
        payload = json.loads(_request_bytes(f"{SEARCH_ENDPOINT}?{params}", timeout))
        body = payload.get("body", []) if isinstance(payload, dict) else []
        for item in body:
            if not isinstance(item, dict):
                continue
            guide_id = str(item.get("guide_id", ""))
            if str(item.get("type", "")).casefold() != "audio" or not guide_id.startswith("s"):
                continue
            image = normalize_tunein_image_url(str(item.get("image", "")))
            if not image:
                continue
            name = str(item.get("text") or item.get("name") or "").strip()
            candidates_by_id[guide_id] = SearchCandidate(
                name=name or "Unnamed result",
                image_url=image,
                guide_id=guide_id,
                subtext=str(item.get("subtext", "") or "").strip(),
                score=_candidate_score(query, name),
            )
    except Exception:
        pass

    # The official public sitemap is the authoritative name/id inventory and
    # includes stations missing from Search.ashx (for example Radio Paradise).
    try:
        catalog_matches = _catalog_matches(query)
        if catalog_matches:
            best_score = catalog_matches[0][1]
            eligible = [
                item
                for item in catalog_matches
                if item[1] >= 0.72 and item[1] >= best_score - 0.08
            ][:3]
            for catalog_station, _score in eligible:
                if catalog_station.guide_id in candidates_by_id:
                    continue
                described = _describe_station(
                    catalog_station.guide_id, query, catalog_station.name, timeout
                )
                if described is not None:
                    candidates_by_id[described.guide_id] = described
    except Exception:
        pass

    candidates = list(candidates_by_id.values())
    candidates.sort(
        key=lambda item: (_query_token_recall(query, item.name), item.score),
        reverse=True,
    )
    return candidates


def apply_best_candidate(entry: MapEntry, candidates: list[SearchCandidate]) -> None:
    entry.candidates = candidates
    if not candidates:
        entry.status = "no TuneIn result"
        return
    query = build_search_query(entry.station)
    selected = next(
        (
            candidate
            for candidate in candidates
            if candidate.score >= 0.72
            and _query_token_recall(query, candidate.name) >= 0.50
        ),
        None,
    )
    if selected is None:
        entry.remote_url = ""
        entry.status = f"no reliable match (best {candidates[0].score:.0%})"
    elif selected.score >= 0.995:
        entry.remote_url = selected.image_url
        entry.status = "TuneIn exact"
    else:
        entry.remote_url = selected.image_url
        entry.status = f"REVIEW TuneIn {selected.score:.0%}"


def _print_report(path: Path, entries: Sequence[MapEntry], report: LoadReport) -> None:
    print(
        f"Loaded {len(entries)} stations from {path} "
        f"({report.delimiter_name}, {report.encoding})."
    )
    if report.duplicate_rows:
        print(f"Warning: skipped {report.duplicate_rows} duplicate station row(s).", file=sys.stderr)
    if report.replacement_character_rows:
        rows = ", ".join(map(str, report.replacement_character_rows))
        print(
            "Warning: the input already contains the Unicode replacement character "
            f"(U+FFFD) in row(s): {rows}. Re-export these names from the original file.",
            file=sys.stderr,
        )


def cli_main(args: argparse.Namespace) -> int:
    input_path = Path(args.input)
    entries, report = load_playlist(input_path)
    _print_report(input_path, entries, report)
    if args.existing_map:
        matches = merge_existing_map(entries, load_existing_map(Path(args.existing_map)))
        print(f"Merged local filenames for {matches} station(s).")

    if args.search:
        missing = [entry for entry in entries if not entry.remote_url]
        for index, entry in enumerate(missing, start=1):
            print(f"TuneIn search {index}/{len(missing)}: {entry.station}")
            try:
                apply_best_candidate(entry, search_tunein(entry.station))
            except Exception as exc:  # network/API failures must not destroy existing data
                entry.status = f"search error: {exc}"
                print(f"  Warning: {exc}", file=sys.stderr)
            if index < len(missing):
                time.sleep(SEARCH_DELAY_SECONDS)

    output_path = Path(args.output) if args.output else input_path.with_name("map.generated.csv")
    write_map(output_path, entries)
    review = sum(entry.status.startswith("REVIEW") for entry in entries)
    missing = sum(not entry.remote_url for entry in entries)
    print(f"Wrote {len(entries)} rows to {output_path}")
    print(f"Remote URL missing: {missing}; TuneIn results requiring review: {review}")
    return 0


def gui_main(initial_input: str | None = None) -> int:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    class Application:
        def __init__(self, root: tk.Tk) -> None:
            self.root = root
            self.root.title(APP_NAME)
            self.root.geometry("1280x700")
            self.entries: list[MapEntry] = []
            self.input_path: Path | None = None
            self.project_path: Path | None = None
            self.data_dir: Path | None = None
            self.staging_dir: Path | None = None
            self.cache: dict[str, list[SearchCandidate]] = {}
            self.events: queue.Queue[tuple] = queue.Queue()
            self.searching = False
            self.downloading = False
            self.stop_requested = False

            toolbar = ttk.Frame(root, padding=8)
            toolbar.pack(fill="x")
            ttk.Button(toolbar, text="Load playlist.csv", command=self.load_playlist_dialog).pack(
                side="left", padx=3
            )
            ttk.Button(toolbar, text="Load existing map.csv", command=self.load_map_dialog).pack(
                side="left", padx=3
            )
            self.search_button = ttk.Button(
                toolbar, text="Find station logos on TuneIn", command=self.start_search
            )
            self.search_button.pack(side="left", padx=3)
            ttk.Button(toolbar, text="Choose TuneIn result…", command=self.choose_candidate).pack(
                side="left", padx=3
            )
            ttk.Button(toolbar, text="Add image URL…", command=self.add_manual_url).pack(
                side="left", padx=3
            )
            self.download_button = ttk.Button(
                toolbar,
                text="Download logos to staging",
                command=self.start_download,
            )
            self.download_button.pack(side="left", padx=3)
            self.resize_var = tk.BooleanVar(value=True)
            ttk.Checkbutton(
                toolbar,
                text=f"Resize to {DEFAULT_LOCAL_LOGO_SIZE}×{DEFAULT_LOCAL_LOGO_SIZE}",
                variable=self.resize_var,
            ).pack(side="left", padx=(8, 3))
            ttk.Button(toolbar, text="Save map.csv", command=self.save_dialog).pack(
                side="right", padx=3
            )

            export_toolbar = ttk.Frame(root, padding=(8, 0, 8, 6))
            export_toolbar.pack(fill="x")
            ttk.Button(export_toolbar, text="Choose data folder…", command=self.choose_data_dir).pack(
                side="left", padx=3
            )
            ttk.Button(export_toolbar, text="Export selected logos + fallback icons", command=self.export_dialog).pack(
                side="left", padx=3
            )
            self.budget_var = tk.StringVar(value="Select the project's data folder to check the LittleFS budget.")
            ttk.Label(export_toolbar, textvariable=self.budget_var).pack(side="left", padx=10)

            project_toolbar = ttk.Frame(root, padding=(8, 0, 8, 6))
            project_toolbar.pack(fill="x")
            ttk.Button(
                project_toolbar, text="Save work…", command=self.save_project_dialog
            ).pack(side="left", padx=3)
            ttk.Button(
                project_toolbar, text="Resume work…", command=self.load_project_dialog
            ).pack(side="left", padx=3)
            ttk.Label(
                project_toolbar,
                text="Save work keeps all edits and TuneIn choices; map.csv is for LittleFS.",
            ).pack(side="left", padx=10)

            tree_frame = ttk.Frame(root, padding=(8, 0))
            tree_frame.pack(fill="both", expand=True)
            columns = ("include", "station", "remote", "local", "size", "status")
            self.tree = ttk.Treeview(tree_frame, columns=columns, show="headings", selectmode="browse")
            self.tree.heading("include", text="Use")
            self.tree.heading("station", text="Station")
            self.tree.heading("remote", text="Remote image URL")
            self.tree.heading("local", text="LittleFS filename")
            self.tree.heading("size", text="Size")
            self.tree.heading("status", text="Status")
            self.tree.column("include", width=48, stretch=False, anchor="center")
            self.tree.column("station", width=250, stretch=False)
            self.tree.column("remote", width=450)
            self.tree.column("local", width=180, stretch=False)
            self.tree.column("size", width=80, stretch=False, anchor="e")
            self.tree.column("status", width=210, stretch=False)
            y_scroll = ttk.Scrollbar(tree_frame, orient="vertical", command=self.tree.yview)
            x_scroll = ttk.Scrollbar(tree_frame, orient="horizontal", command=self.tree.xview)
            self.tree.configure(yscrollcommand=y_scroll.set, xscrollcommand=x_scroll.set)
            self.tree.grid(row=0, column=0, sticky="nsew")
            y_scroll.grid(row=0, column=1, sticky="ns")
            x_scroll.grid(row=1, column=0, sticky="ew")
            tree_frame.rowconfigure(0, weight=1)
            tree_frame.columnconfigure(0, weight=1)
            self.tree.bind("<<TreeviewSelect>>", self.on_select)
            self.tree.bind("<Button-1>", self.on_tree_click)
            self.tree.bind("<Double-1>", self.on_tree_double_click)

            editor = ttk.LabelFrame(root, text="Selected station", padding=8)
            editor.pack(fill="x", padx=8, pady=6)
            ttk.Label(editor, text="Remote URL:").grid(row=0, column=0, sticky="w")
            self.remote_var = tk.StringVar()
            ttk.Entry(editor, textvariable=self.remote_var).grid(
                row=0, column=1, sticky="ew", padx=(5, 12)
            )
            ttk.Label(editor, text="Local override:").grid(row=0, column=2, sticky="w")
            self.local_var = tk.StringVar()
            ttk.Entry(editor, textvariable=self.local_var, width=28).grid(
                row=0, column=3, sticky="ew", padx=5
            )
            ttk.Button(editor, text="Apply", command=self.apply_editor).grid(row=0, column=4, padx=5)
            ttk.Button(editor, text="Open image", command=self.open_selected_image).grid(
                row=0, column=5, padx=5
            )
            ttk.Button(editor, text="Clear logo", command=self.clear_selected_logo).grid(
                row=0, column=6, padx=5
            )
            editor.columnconfigure(1, weight=1)

            self.status_var = tk.StringVar(value="Load a playlist to begin.")
            ttk.Label(root, textvariable=self.status_var, padding=(10, 2, 10, 8)).pack(fill="x")
            self.root.after(100, self.poll_events)
            if initial_input:
                self.load_playlist_path(Path(initial_input))

        def selected_index(self) -> int | None:
            selection = self.tree.selection()
            if not selection:
                return None
            try:
                return int(selection[0])
            except ValueError:
                return None

        def refresh_tree(self, keep_selection: int | None = None) -> None:
            self.tree.delete(*self.tree.get_children())
            for index, entry in enumerate(self.entries):
                self.tree.insert(
                    "",
                    "end",
                    iid=str(index),
                    values=(
                        "☑" if entry.include_logo else "☐",
                        entry.station,
                        entry.remote_url,
                        entry.local_file,
                        format_size(entry.local_size) if entry.local_size else "",
                        entry.status,
                    ),
                )
            if keep_selection is not None and 0 <= keep_selection < len(self.entries):
                self.tree.selection_set(str(keep_selection))
                self.tree.see(str(keep_selection))

        def on_tree_click(self, event) -> None:
            if self.tree.identify_column(event.x) != "#1":
                return
            row = self.tree.identify_row(event.y)
            if not row or self.downloading or self.searching:
                return
            entry = self.entries[int(row)]
            entry.include_logo = not entry.include_logo
            self.refresh_tree(int(row))
            self.refresh_budget()

        def on_tree_double_click(self, event) -> None:
            if self.tree.identify_column(event.x) != "#1":
                self.choose_candidate()

        def choose_data_dir(self) -> bool:
            initial = self.data_dir or (self.input_path.parent if self.input_path else Path.cwd())
            directory = filedialog.askdirectory(
                title="Select the project's data folder (not images/stations)",
                initialdir=str(initial), mustexist=True,
            )
            if not directory:
                return False
            try:
                self.data_dir = validate_data_dir(Path(directory))
            except ValueError as exc:
                messagebox.showerror(APP_NAME, str(exc))
                return False
            self.refresh_budget()
            return True

        def refresh_budget(self) -> None:
            if not self.data_dir:
                self.budget_var.set("Select the project's data folder to check the LittleFS budget.")
                return
            try:
                plan = build_export_plan(self.data_dir, self.entries, self.staging_dir)
            except (OSError, ValueError) as exc:
                self.budget_var.set(f"Export not ready: {exc}")
                return
            remaining = plan.limit_bytes - plan.total_bytes
            self.budget_var.set(
                f"Other data: {format_size(plan.nonstation_bytes)} | "
                f"logos + fallback + map: {format_size(plan.stations_bytes)} | "
                f"total: {format_size(plan.total_bytes)} / {format_size(plan.limit_bytes)} "
                f"(80% LittleFS) | remaining: {format_size(max(remaining, 0))}"
                if remaining >= 0 else
                f"OVER LIMIT by {format_size(-remaining)} — uncheck some logos."
            )

        def export_dialog(self) -> None:
            if not self.entries:
                messagebox.showinfo(APP_NAME, "Load a playlist first.")
                return
            if self.downloading or self.searching:
                messagebox.showinfo(APP_NAME, "Wait for the current search/download to finish.")
                return
            if not self.apply_editor():
                return
            if not self.data_dir and not self.choose_data_dir():
                return
            try:
                plan = build_export_plan(self.data_dir, self.entries, self.staging_dir)
                if plan.total_bytes > plan.limit_bytes:
                    raise ValueError(
                        f"LittleFS budget exceeded by {format_size(plan.total_bytes - plan.limit_bytes)}. "
                        "Uncheck unneeded station logos in the Use column."
                    )
                if not messagebox.askyesno(
                    APP_NAME,
                    f"Export to {plan.data_dir / 'images' / 'stations'}?\n\n"
                    f"Other data: {format_size(plan.nonstation_bytes)}\n"
                    f"New stations folder (logos + 4 fallback icons + map.csv): {format_size(plan.stations_bytes)}\n"
                    f"Total: {format_size(plan.total_bytes)} / {format_size(plan.limit_bytes)}\n\n"
                    "The old stations folder will be kept as a backup outside data.",
                ):
                    return
                backup = export_to_data(plan, Path(__file__).resolve().parent / "output" / "backups")
            except (OSError, ValueError) as exc:
                messagebox.showerror(APP_NAME, f"Export failed; old artwork was preserved:\n{exc}")
                self.refresh_budget()
                return
            self.refresh_budget()
            self.status_var.set(f"Exported {len(plan.image_sources) - len(FALLBACK_NAMES)} station logo(s) and 4 fallback icons.")
            messagebox.showinfo(APP_NAME, f"Export complete.\n\nBackup: {backup or 'No previous stations folder'}")

        def load_playlist_dialog(self) -> None:
            filename = filedialog.askopenfilename(
                title="Open playlist",
                filetypes=(("CSV/TSV files", "*.csv *.tsv *.txt"), ("All files", "*.*")),
            )
            if filename:
                self.load_playlist_path(Path(filename))

        def load_playlist_path(self, path: Path) -> None:
            try:
                self.entries, report = load_playlist(path)
            except Exception as exc:
                messagebox.showerror(APP_NAME, f"Could not load playlist:\n{exc}")
                return
            self.input_path = path
            self.project_path = None
            self.staging_dir = None
            self.cache.clear()
            self.refresh_tree()
            self.refresh_budget()
            missing = sum(not entry.remote_url for entry in self.entries)
            warning = ""
            if report.replacement_character_rows:
                warning = (
                    f" Warning: damaged replacement characters in {len(report.replacement_character_rows)} row(s)."
                )
            self.status_var.set(
                f"Loaded {len(self.entries)} stations ({report.delimiter_name}, {report.encoding}); "
                f"{missing} logo URL(s) missing.{warning}"
            )
            sibling_map = path.with_name("map.csv")
            if sibling_map.exists() and sibling_map != path:
                matches = merge_existing_map(self.entries, load_existing_map(sibling_map))
                self.refresh_tree()
                self.refresh_budget()
                self.status_var.set(self.status_var.get() + f" Auto-merged {matches} local map entries.")

        def load_map_dialog(self) -> None:
            if not self.entries:
                messagebox.showinfo(APP_NAME, "Load the playlist first.")
                return
            filename = filedialog.askopenfilename(
                title="Open existing map.csv",
                filetypes=(("CSV/TSV files", "*.csv *.tsv *.txt"), ("All files", "*.*")),
            )
            if not filename:
                return
            try:
                matches = merge_existing_map(self.entries, load_existing_map(Path(filename)))
            except Exception as exc:
                messagebox.showerror(APP_NAME, f"Could not load map:\n{exc}")
                return
            self.refresh_tree()
            self.status_var.set(f"Merged {matches} station(s) from the existing map.")
            self.refresh_budget()

        def save_dialog(self) -> None:
            if not self.entries:
                messagebox.showinfo(APP_NAME, "Nothing to save.")
                return
            if self.downloading or self.searching:
                messagebox.showinfo(APP_NAME, "Wait for the current search/download to finish.")
                return
            if not self.apply_editor():
                return
            initial_dir = str(self.input_path.parent) if self.input_path else ""
            filename = filedialog.asksaveasfilename(
                title="Save three-column map.csv",
                initialdir=initial_dir,
                initialfile="map.csv",
                defaultextension=".csv",
                filetypes=(("TAB-separated map", "*.csv"), ("All files", "*.*")),
            )
            if not filename:
                return
            try:
                write_map(Path(filename), self.entries, selected_only=True)
            except Exception as exc:
                messagebox.showerror(APP_NAME, f"Could not save map:\n{exc}")
                return
            missing = sum(not entry.remote_url for entry in self.entries)
            review = sum(entry.status.startswith("REVIEW") for entry in self.entries)
            self.status_var.set(f"Saved {len(self.entries)} rows. Missing: {missing}; review: {review}.")

        def save_project_dialog(self) -> None:
            if not self.entries:
                messagebox.showinfo(APP_NAME, "Nothing to save.")
                return
            if self.downloading or self.searching:
                messagebox.showinfo(APP_NAME, "Wait for the current search/download to finish.")
                return
            if not self.apply_editor():
                return
            filename = filedialog.asksaveasfilename(
                title="Save editing progress",
                initialdir=str((self.project_path or self.input_path or Path.cwd()).parent),
                initialfile=self.project_path.name if self.project_path else "station-map.project.json",
                defaultextension=".json",
                filetypes=(("TuneIn map project", "*.json"), ("All files", "*.*")),
            )
            if not filename:
                return
            try:
                write_project(Path(filename), self.entries, self.input_path,
                              self.resize_var.get(), self.staging_dir)
            except Exception as exc:
                messagebox.showerror(APP_NAME, f"Could not save editing progress:\n{exc}")
                return
            self.project_path = Path(filename)
            self.status_var.set(f"Saved editing progress: {self.project_path}")

        def load_project_dialog(self) -> None:
            if self.downloading or self.searching:
                messagebox.showinfo(APP_NAME, "Wait for the current search/download to finish.")
                return
            filename = filedialog.askopenfilename(
                title="Resume editing progress",
                initialdir=str((self.project_path or self.input_path or Path.cwd()).parent),
                filetypes=(("TuneIn map project", "*.json"), ("All files", "*.*")),
            )
            if not filename:
                return
            try:
                entries, playlist_path, resize_to_120, staging_dir = load_project(Path(filename))
            except Exception as exc:
                messagebox.showerror(APP_NAME, f"Could not load editing progress:\n{exc}")
                return
            self.entries = entries
            self.input_path = playlist_path
            self.project_path = Path(filename)
            self.resize_var.set(resize_to_120)
            self.staging_dir = staging_dir
            self.cache = {station_key(entry.station): entry.candidates for entry in entries}
            self.refresh_tree(0 if entries else None)
            if entries:
                self.on_select()
            else:
                self.remote_var.set("")
                self.local_var.set("")
            self.status_var.set(f"Resumed {len(entries)} stations from {self.project_path}.")
            self.refresh_budget()

        def on_select(self, _event=None) -> None:
            index = self.selected_index()
            if index is None:
                return
            entry = self.entries[index]
            self.local_var.set(entry.local_file)
            self.remote_var.set(entry.remote_url)

        def apply_editor(self) -> bool:
            index = self.selected_index()
            if index is None:
                return True
            entry = self.entries[index]
            local_file = self.local_var.get().strip()
            if local_file and Path(local_file).suffix.casefold() != ".png":
                messagebox.showerror(
                    APP_NAME, "The LittleFS logo filename must end in .png."
                )
                return False
            try:
                remote_url = (
                    validate_manual_image_url(self.remote_var.get())
                    if self.remote_var.get().strip()
                    else ""
                )
            except ValueError as exc:
                messagebox.showerror(APP_NAME, str(exc))
                return False
            if local_file == entry.local_file and remote_url == entry.remote_url:
                return True
            local_was_edited = local_file != entry.local_file
            select_image_url(entry, remote_url)
            if local_was_edited:
                entry.local_file = local_file
                if local_file:
                    entry.include_logo = True
            entry.status = "edited"
            self.refresh_tree(index)
            self.on_select()
            self.refresh_budget()
            return True

        def clear_selected_logo(self) -> None:
            index = self.selected_index()
            if index is None:
                messagebox.showinfo(APP_NAME, "Select a station first.")
                return
            if self.downloading or self.searching:
                messagebox.showinfo(APP_NAME, "Wait for the current search/download to finish.")
                return
            entry = self.entries[index]
            clear_logo(entry)
            self.remote_var.set("")
            self.local_var.set("")
            self.refresh_tree(index)
            self.refresh_budget()
            self.status_var.set(
                f"Logo references cleared for {entry.station}. Save map.csv to keep this change."
            )

        def add_manual_url(self) -> None:
            index = self.selected_index()
            if index is None:
                messagebox.showinfo(APP_NAME, "Select a station first.")
                return
            entry = self.entries[index]

            dialog = tk.Toplevel(self.root)
            dialog.title(f"Add image URL — {entry.station}")
            dialog.geometry("820x175")
            dialog.resizable(True, False)
            dialog.transient(self.root)
            dialog.grab_set()

            content = ttk.Frame(dialog, padding=12)
            content.pack(fill="both", expand=True)
            ttk.Label(content, text=entry.station, font=("TkDefaultFont", 10, "bold")).grid(
                row=0, column=0, columnspan=3, sticky="w", pady=(0, 8)
            )
            ttk.Label(content, text="Image URL:").grid(row=1, column=0, sticky="w")
            url_var = tk.StringVar(value=entry.remote_url)
            url_entry = ttk.Entry(content, textvariable=url_var)
            url_entry.grid(row=1, column=1, columnspan=2, sticky="ew", padx=(8, 0))
            ttk.Label(
                content,
                text=(
                    "Paste a PNG or JPG image URL. The downloaded image is "
                    "checked and saved locally as PNG."
                ),
            ).grid(row=2, column=1, columnspan=2, sticky="w", pady=(5, 10))
            content.columnconfigure(1, weight=1)

            buttons = ttk.Frame(content)
            buttons.grid(row=3, column=0, columnspan=3, sticky="e")

            def normalized_url() -> str | None:
                try:
                    return validate_manual_image_url(url_var.get())
                except ValueError as exc:
                    messagebox.showerror(APP_NAME, str(exc), parent=dialog)
                    return None

            def open_url() -> None:
                url = normalized_url()
                if url:
                    url_var.set(url)
                    self.open_validated_image(url, index)

            def use_url() -> None:
                url = normalized_url()
                if not url:
                    return
                select_image_url(entry, url)
                entry.status = "manual URL"
                self.remote_var.set(url)
                self.local_var.set(entry.local_file)
                self.refresh_tree(index)
                self.refresh_budget()
                self.status_var.set(f"Manual image URL added for {entry.station}.")
                dialog.destroy()

            ttk.Button(buttons, text="Cancel", command=dialog.destroy).pack(side="right", padx=3)
            ttk.Button(buttons, text="Open image", command=open_url).pack(side="right", padx=3)
            ttk.Button(buttons, text="Use URL", command=use_url).pack(side="right", padx=3)
            dialog.bind("<Return>", lambda _event: use_url())
            dialog.bind("<Escape>", lambda _event: dialog.destroy())
            url_entry.focus_set()
            url_entry.selection_range(0, "end")

        def open_selected_image(self) -> None:
            url = self.remote_var.get().strip()
            if url:
                self.open_validated_image(url, self.selected_index())

        def open_validated_image(self, url: str, index: int | None = None) -> None:
            self.status_var.set("Checking the image before opening it…")
            threading.Thread(target=self.preview_worker, args=(url, index), daemon=True).start()

        def preview_worker(self, url: str, index: int | None) -> None:
            try:
                logo = download_logo(url, timeout=10.0)
                self.events.put(("preview_result", url, logo.source_url, index))
            except Exception as exc:
                self.events.put(("preview_error", str(exc)))

        def default_download_dir(self) -> Path:
            return Path(__file__).resolve().parent / "output" / "staging"

        def start_download(self) -> None:
            if self.downloading:
                return
            if self.searching:
                messagebox.showinfo(APP_NAME, "Stop the TuneIn search before downloading logos.")
                return
            if not self.apply_editor():
                return
            jobs = [
                (index, entry.station, entry.remote_url, entry.local_file)
                for index, entry in enumerate(self.entries)
                if entry.include_logo and entry.remote_url
            ]
            if not jobs:
                messagebox.showinfo(APP_NAME, "There are no selected remote logos to download.")
                return
            if self.staging_dir is None:
                self.staging_dir = self.default_download_dir() / uuid.uuid4().hex
            output_dir = self.staging_dir
            output_dir.mkdir(parents=True, exist_ok=True)
            target_size = DEFAULT_LOCAL_LOGO_SIZE if self.resize_var.get() else 0
            self.downloading = True
            self.download_button.configure(state="disabled", text="Downloading logos…")
            self.search_button.configure(state="disabled")
            threading.Thread(
                target=self.download_worker,
                args=(jobs, output_dir, target_size),
                daemon=True,
            ).start()

        def download_worker(
            self,
            jobs: list[tuple[int, str, str, str]],
            output_dir: Path,
            target_size: int,
        ) -> None:
            preexisting_names = {
                path.name.casefold() for path in output_dir.iterdir() if path.is_file()
            }
            used: set[str] = set(preexisting_names)
            accounted_paths: set[str] = set()
            downloaded_by_station: dict[str, tuple[str, int, int, int, str]] = {}
            total_bytes = 0
            successful = 0
            failed = 0
            non_300 = 0
            expected_dimensions = (
                (target_size, target_size) if target_size else (300, 300)
            )
            for position, (index, station, remote_url, local_file) in enumerate(jobs, start=1):
                self.events.put(("download_progress", position, len(jobs), station))
                requested_name = Path(local_file).name if local_file else ""
                existing_path = output_dir / requested_name if requested_name else None
                if (
                    existing_path
                    and requested_name.casefold() in preexisting_names
                    and existing_path.is_file()
                ):
                    try:
                        existing_data = existing_path.read_bytes()
                        normalized = convert_image_to_png(
                            existing_data, str(existing_path), target_size
                        )
                        result = "existing local file"
                        destination = existing_path
                        if existing_path.suffix.casefold() != ".png":
                            filename = unique_logo_filename(
                                station, ".png", requested_name, used
                            )
                            destination = output_dir / filename
                            result = "converted local file"
                        if destination != existing_path or normalized.data != existing_data:
                            temporary = destination.with_name(destination.name + ".part")
                            temporary.write_bytes(normalized.data)
                            temporary.replace(destination)
                            if destination == existing_path:
                                result = "normalized local file"
                        used.add(destination.name.casefold())
                        path_key = str(destination.resolve()).casefold()
                        if path_key not in accounted_paths:
                            accounted_paths.add(path_key)
                            total_bytes += len(normalized.data)
                        successful += 1
                        if (normalized.width, normalized.height) != expected_dimensions:
                            non_300 += 1
                        self.events.put(
                            (
                                "download_result",
                                index,
                                destination.name,
                                len(normalized.data),
                                normalized.width,
                                normalized.height,
                                result,
                                "",
                            )
                        )
                        continue
                    except (OSError, ValueError):
                        pass
                station_id = _tunein_station_id(remote_url)
                remote_key = (
                    f"tunein:{station_id}"
                    if station_id
                    else normalize_tunein_image_url(remote_url).casefold()
                )
                previous = downloaded_by_station.get(remote_key)
                if previous:
                    filename, size, width, height, resolved_url = previous
                    successful += 1
                    if (width, height) != expected_dimensions:
                        non_300 += 1
                    self.events.put(
                        (
                            "download_result",
                            index,
                            filename,
                            size,
                            width,
                            height,
                            "shared downloaded file",
                            resolved_url,
                        )
                    )
                    continue
                try:
                    logo = download_logo(remote_url)
                    if target_size:
                        logo = resize_downloaded_logo(logo, target_size)
                    filename = unique_logo_filename(
                        station, logo.extension, requested_name, used
                    )
                    destination = output_dir / filename
                    temporary = destination.with_name(destination.name + ".part")
                    temporary.write_bytes(logo.data)
                    temporary.replace(destination)
                    path_key = str(destination.resolve()).casefold()
                    if path_key not in accounted_paths:
                        accounted_paths.add(path_key)
                        total_bytes += len(logo.data)
                    downloaded_by_station[remote_key] = (
                        filename,
                        len(logo.data),
                        logo.width,
                        logo.height,
                        logo.source_url,
                    )
                    successful += 1
                    if (logo.width, logo.height) != expected_dimensions:
                        non_300 += 1
                    self.events.put(
                        (
                            "download_result",
                            index,
                            filename,
                            len(logo.data),
                            logo.width,
                            logo.height,
                            "downloaded + resized" if target_size else "downloaded",
                            logo.source_url,
                        )
                    )
                except Exception as exc:
                    failed += 1
                    self.events.put(("download_error", index, str(exc)))
            self.events.put(
                (
                    "download_done",
                    output_dir,
                    successful,
                    failed,
                    total_bytes,
                    non_300,
                    target_size,
                )
            )

        def start_search(self) -> None:
            if self.searching:
                self.stop_requested = True
                self.status_var.set("Stopping after the current TuneIn request…")
                return
            if self.downloading:
                messagebox.showinfo(APP_NAME, "Wait for the logo download to finish.")
                return
            missing = [
                index
                for index, entry in enumerate(self.entries)
                if not entry.remote_url and entry.status != "no logo (manual)"
            ]
            if not missing:
                messagebox.showinfo(APP_NAME, "Every station already has a remote image URL.")
                return
            self.searching = True
            self.stop_requested = False
            self.search_button.configure(text="Stop search")
            threading.Thread(target=self.search_worker, args=(missing,), daemon=True).start()

        def search_worker(self, indexes: list[int]) -> None:
            for position, index in enumerate(indexes, start=1):
                if self.stop_requested:
                    break
                entry = self.entries[index]
                self.events.put(("progress", position, len(indexes), entry.station))
                try:
                    candidates = search_tunein(entry.station)
                    self.cache[station_key(entry.station)] = candidates
                    self.events.put(("result", index, candidates))
                except Exception as exc:
                    self.events.put(("error", index, str(exc)))
                if position < len(indexes):
                    time.sleep(SEARCH_DELAY_SECONDS)
            self.events.put(("done",))

        def poll_events(self) -> None:
            try:
                while True:
                    event = self.events.get_nowait()
                    if event[0] == "progress":
                        _, current, total, station = event
                        self.status_var.set(f"TuneIn search {current}/{total}: {station}")
                    elif event[0] == "result":
                        _, index, candidates = event
                        apply_best_candidate(self.entries[index], candidates)
                        self.refresh_tree(index)
                    elif event[0] == "error":
                        _, index, error = event
                        self.entries[index].status = f"search error: {error}"
                        self.refresh_tree(index)
                    elif event[0] == "done":
                        self.searching = False
                        self.search_button.configure(text="Find station logos on TuneIn")
                        missing = sum(not entry.remote_url for entry in self.entries)
                        review = sum(entry.status.startswith("REVIEW") for entry in self.entries)
                        self.status_var.set(f"Search complete. Missing: {missing}; review manually: {review}.")
                    elif event[0] == "download_progress":
                        _, current, total, station = event
                        self.status_var.set(f"Downloading logo {current}/{total}: {station}")
                    elif event[0] == "download_result":
                        _, index, filename, size, width, height, result, resolved_url = event
                        entry = self.entries[index]
                        entry.local_file = filename
                        entry.local_size = size
                        if resolved_url:
                            entry.remote_url = resolved_url
                        entry.status = f"{result}: {width}×{height}"
                        self.refresh_tree(index)
                    elif event[0] == "download_error":
                        _, index, error = event
                        mark_download_failed(self.entries[index], error)
                        self.refresh_tree(index)
                    elif event[0] == "preview_result":
                        _, requested_url, resolved_url, index = event
                        if index is not None and 0 <= index < len(self.entries):
                            entry = self.entries[index]
                            if entry.remote_url == requested_url and resolved_url != requested_url:
                                entry.remote_url = resolved_url
                                self.refresh_tree(index)
                                self.on_select()
                        webbrowser.open(resolved_url)
                        self.status_var.set(f"Opened verified image: {resolved_url}")
                    elif event[0] == "preview_error":
                        _, error = event
                        self.status_var.set("Image preview failed.")
                        messagebox.showerror(APP_NAME, f"Could not open an image:\n{error}")
                    elif event[0] == "download_done":
                        (
                            _,
                            output_dir,
                            successful,
                            failed,
                            total_bytes,
                            non_300,
                            target_size,
                        ) = event
                        self.downloading = False
                        self.download_button.configure(
                            state="normal", text="Download logos to staging"
                        )
                        self.search_button.configure(state="normal")
                        self.refresh_budget()
                        summary = (
                            f"Staged: {successful} logo(s), {failed} failed.\n\n"
                            f"Downloaded image total: {format_size(total_bytes)} ({total_bytes:,} bytes)\n"
                            f"Staging folder: {output_dir}\n"
                            "Choose the data folder, uncheck unneeded logos, then export."
                        )
                        if target_size:
                            summary += f"\nOutput size: {target_size}×{target_size} PNG"
                        elif non_300:
                            summary += (
                                f"\n\nNote: {non_300} image(s) are not exactly 300×300; "
                                "their actual dimensions are shown in the Status column."
                            )
                        if failed:
                            summary += (
                                "\n\nFailed rows have no local logo filename; "
                                "fix them and download again if needed."
                            )
                        self.status_var.set(
                            f"Downloaded/reused {successful} logo(s), {failed} failed; "
                            f"image total: {format_size(total_bytes)}. Ready for budget review."
                        )
                        if failed:
                            messagebox.showwarning(APP_NAME, summary)
                        else:
                            messagebox.showinfo(APP_NAME, summary)
            except queue.Empty:
                pass
            self.root.after(100, self.poll_events)

        def choose_candidate(self) -> None:
            index = self.selected_index()
            if index is None:
                messagebox.showinfo(APP_NAME, "Select a station first.")
                return
            entry = self.entries[index]
            candidates = self.cache.get(station_key(entry.station), entry.candidates)
            if not candidates:
                try:
                    self.status_var.set(f"Searching TuneIn for {entry.station}…")
                    self.root.update_idletasks()
                    candidates = search_tunein(entry.station)
                    self.cache[station_key(entry.station)] = candidates
                except Exception as exc:
                    messagebox.showerror(APP_NAME, f"TuneIn search failed:\n{exc}")
                    return
            if not candidates:
                messagebox.showinfo(APP_NAME, "TuneIn returned no result with artwork.")
                return

            dialog = tk.Toplevel(self.root)
            dialog.title(f"TuneIn results — {entry.station}")
            dialog.geometry("920x420")
            dialog.transient(self.root)
            result_tree = ttk.Treeview(
                dialog,
                columns=("name", "details", "score", "guide", "url"),
                show="headings",
                selectmode="browse",
            )
            for column, title, width in (
                ("name", "TuneIn station", 280),
                ("details", "Description / now playing", 250),
                ("score", "Match", 70),
                ("guide", "Guide ID", 90),
                ("url", "Image URL", 350),
            ):
                result_tree.heading(column, text=title)
                result_tree.column(column, width=width, stretch=column in {"name", "url"})
            for candidate_index, candidate in enumerate(candidates):
                result_tree.insert(
                    "",
                    "end",
                    iid=str(candidate_index),
                    values=(
                        candidate.name,
                        candidate.subtext,
                        f"{candidate.score:.0%}",
                        candidate.guide_id,
                        candidate.image_url,
                    ),
                )
            result_tree.pack(fill="both", expand=True, padx=8, pady=8)
            result_tree.selection_set("0")

            buttons = ttk.Frame(dialog, padding=(8, 0, 8, 8))
            buttons.pack(fill="x")

            def selected_candidate() -> SearchCandidate | None:
                selected = result_tree.selection()
                return candidates[int(selected[0])] if selected else None

            def use_result() -> None:
                candidate = selected_candidate()
                if candidate is None:
                    return
                select_image_url(entry, candidate.image_url)
                entry.status = "TuneIn selected"
                entry.candidates = candidates
                self.refresh_tree(index)
                self.on_select()
                dialog.destroy()

            def open_result() -> None:
                candidate = selected_candidate()
                if candidate:
                    self.open_validated_image(candidate.image_url, index)

            ttk.Button(buttons, text="Use selected result", command=use_result).pack(side="right", padx=3)
            ttk.Button(buttons, text="Open image", command=open_result).pack(side="right", padx=3)
            ttk.Button(buttons, text="Cancel", command=dialog.destroy).pack(side="right", padx=3)
            result_tree.bind("<Double-1>", lambda _event: use_result())

    root = tk.Tk()
    Application(root)
    root.mainloop()
    return 0


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", nargs="?", help="playlist.csv/TSV to convert")
    parser.add_argument("--existing-map", help="optional existing 2- or 3-column map.csv")
    parser.add_argument("--output", help="output file (default: map.generated.csv beside input)")
    parser.add_argument(
        "--search",
        action="store_true",
        help="query the unofficial TuneIn search endpoint for missing logo URLs",
    )
    parser.add_argument("--gui", action="store_true", help="start the desktop GUI")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    if args.gui or not args.input:
        return gui_main(args.input)
    return cli_main(args)


if __name__ == "__main__":
    raise SystemExit(main())
