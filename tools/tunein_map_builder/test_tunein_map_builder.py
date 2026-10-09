import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image

import tunein_map_builder as builder


class MapBuilderTests(unittest.TestCase):
    @staticmethod
    def png(width=300, height=300):
        return builder.encode_png_rgba(
            width, height, bytes((20, 80, 180, 255)) * (width * height)
        )

    @staticmethod
    def jpeg(width=300, height=300):
        result = io.BytesIO()
        Image.new("RGB", (width, height), (20, 80, 180)).save(result, "JPEG")
        return result.getvalue()

    def test_semicolon_playlist_and_leading_dot(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "playlist.csv"
            source.write_text(
                ".181.fm Classic Hits;http://example/stream;0;;"
                "http://cdn-radiotime-logos.tunein.com/s55022q.png\n",
                encoding="utf-8",
            )
            entries, report = builder.load_playlist(source)
            self.assertEqual(report.delimiter, ";")
            self.assertEqual(entries[0].station, "181.fm Classic Hits")
            self.assertEqual(
                entries[0].remote_url,
                "http://cdn-radiotime-logos.tunein.com/s55022q.png",
            )

    def test_existing_map_is_merged_and_three_columns_are_written(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            existing = directory / "old-map.csv"
            existing.write_text("Station One\tlocal.png\n", encoding="utf-8")
            entries = [builder.MapEntry("Station One", remote_url="http://example/logo.png")]
            matched = builder.merge_existing_map(entries, builder.load_existing_map(existing))
            self.assertEqual(matched, 1)
            output = directory / "map.csv"
            builder.write_map(output, entries)
            self.assertEqual(
                output.read_text(encoding="utf-8"),
                "Station One\thttp://example/logo.png\tlocal.png\n",
            )

    def test_project_roundtrip_preserves_editing_progress(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / "station-map.project.json"
            entries = [
                builder.MapEntry(
                    "Station One",
                    local_file="one.png",
                    remote_url="https://example.com/one.jpg",
                    status="downloaded + resized: 120×120",
                    stream_url="http://example.com/stream",
                    local_size=1234,
                    candidates=[builder.SearchCandidate("One", "https://example.com/one.jpg", score=0.9)],
                ),
                builder.MapEntry("Station Two", status="no logo (manual)"),
            ]
            playlist = Path(directory) / "playlist.csv"
            stage = Path(directory) / "staged"
            builder.write_project(project, entries, playlist, True, stage)
            restored, source, resize, staging = builder.load_project(project)
            self.assertEqual(restored, entries)
            self.assertEqual(source, playlist)
            self.assertTrue(resize)
            self.assertEqual(staging, stage)

    def test_budget_excludes_old_stations_and_includes_fallback_map_and_other_data(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = root / "data"
            stations = data / "images" / "stations"
            stations.mkdir(parents=True)
            (data / "options.txt").write_bytes(b"other data")
            (stations / "obsolete.png").write_bytes(b"x" * 1000)
            stage = root / "stage"
            stage.mkdir()
            logo = self.png(4, 4)
            (stage / "chosen.png").write_bytes(logo)
            entries = [
                builder.MapEntry("Chosen", local_file="chosen.png"),
                builder.MapEntry("Skipped", local_file="obsolete.png", include_logo=False),
            ]
            plan = builder.build_export_plan(data, entries, stage)
            expected_station = (
                len(logo) + len(builder.map_csv_bytes(entries, selected_only=True))
                + sum((builder.FALLBACK_DIR / name).stat().st_size for name in builder.FALLBACK_NAMES)
            )
            self.assertEqual(plan.nonstation_bytes, len(b"other data"))
            self.assertEqual(plan.stations_bytes, expected_station)
            self.assertNotIn("obsolete.png", plan.image_sources)
            self.assertEqual(plan.map_entries[1].local_file, "")

    def test_export_replaces_stations_but_keeps_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = root / "data"
            stations = data / "images" / "stations"
            stations.mkdir(parents=True)
            (stations / "old.png").write_bytes(b"old")
            stage = root / "stage"
            stage.mkdir()
            (stage / "new.png").write_bytes(self.png(4, 4))
            entries = [builder.MapEntry("New", local_file="new.png")]
            plan = builder.build_export_plan(data, entries, stage)
            backup = builder.export_to_data(plan, root / "backups")
            self.assertIsNotNone(backup)
            self.assertEqual((backup / "old.png").read_bytes(), b"old")
            self.assertFalse((stations / "old.png").exists())
            self.assertTrue((stations / "new.png").exists())
            for name in builder.FALLBACK_NAMES:
                self.assertTrue((stations / name).exists())
            self.assertEqual((stations / "map.csv").read_text(encoding="utf-8"), "New\t\tnew.png\n")

    def test_export_is_blocked_when_budget_is_exceeded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = root / "data"
            (data / "images").mkdir(parents=True)
            (data / "big.bin").write_bytes(b"x" * (int(builder.littlefs_partition_bytes() * .8) + 1))
            plan = builder.build_export_plan(data, [], None)
            with self.assertRaises(ValueError):
                builder.export_to_data(plan, root / "backups")
            self.assertFalse((data / "images" / "stations").exists())

    def test_invalid_project_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / "station-map.project.json"
            project.write_text('{"format":"wrong","version":1}', encoding="utf-8")
            with self.assertRaises(ValueError):
                builder.load_project(project)

    def test_old_project_version_still_loads(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / "old.project.json"
            project.write_text(
                '{"format":"fusionedge-tunein-map-project","version":1,'
                '"playlist_path":"","resize_to_120":true,'
                '"entries":[{"station":"Old","candidates":[]}]}',
                encoding="utf-8",
            )
            entries, source, resize, staging = builder.load_project(project)
            self.assertEqual(entries[0].station, "Old")
            self.assertTrue(entries[0].include_logo)
            self.assertIsNone(source)
            self.assertTrue(resize)
            self.assertIsNone(staging)

    def test_failed_download_still_writes_partial_map(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "map.csv"
            good = builder.MapEntry("Good", "good.png", "https://example.com/good.jpg")
            failed = builder.MapEntry("Failed", "stale.png", "https://example.com/bad.jpg", local_size=55)
            builder.mark_download_failed(failed, "certificate error")
            builder.write_map(output, [good, failed])
            self.assertEqual(failed.local_size, 0)
            self.assertEqual(
                output.read_text(encoding="utf-8"),
                "Good\thttps://example.com/good.jpg\tgood.png\n"
                "Failed\thttps://example.com/bad.jpg\t\n",
            )

    def test_new_map_order_is_loaded(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "map.csv"
            source.write_text(
                "Station One\thttp://example/logo.png\tlocal.png\n", encoding="utf-8"
            )
            entry = builder.load_existing_map(source)[builder.station_key("Station One")]
            self.assertEqual(entry.remote_url, "http://example/logo.png")
            self.assertEqual(entry.local_file, "local.png")

    def test_explicitly_cleared_map_overrides_playlist_image_url(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "map.csv"
            source.write_text("Station One\t\t\n", encoding="utf-8")
            entry = builder.MapEntry("Station One", remote_url="http://example/bad.png")
            builder.merge_existing_map([entry], builder.load_existing_map(source))
            self.assertEqual(entry.remote_url, "")
            self.assertEqual(entry.local_file, "")

    def test_clear_logo_removes_both_map_references(self):
        entry = builder.MapEntry(
            "Station One", local_file="wrong.png", remote_url="http://example/wrong.jpg",
            local_size=1234,
        )
        builder.clear_logo(entry)
        self.assertEqual((entry.remote_url, entry.local_file, entry.local_size), ("", "", 0))

    def test_replacing_url_discards_previous_downloaded_logo(self):
        entry = builder.MapEntry(
            "Station One", local_file="wrong.png", remote_url="http://example/wrong.png",
            local_size=1234,
        )
        builder.select_image_url(entry, "http://example/correct.jpg")
        self.assertEqual(entry.remote_url, "http://example/correct.jpg")
        self.assertEqual((entry.local_file, entry.local_size), ("", 0))

    def test_tunein_profile_size_is_normalized(self):
        self.assertEqual(
            builder.normalize_tunein_image_url(
                "https://cdn-profiles.tunein.com/s356431/images/logoq.png"
            ),
            "http://cdn-profiles.tunein.com/s356431/images/logoq.png",
        )

    def test_radiotime_https_is_written_as_http(self):
        self.assertEqual(
            builder.normalize_tunein_image_url(
                "https://cdn-radiotime-logos.tunein.com/s55022q.png"
            ),
            "http://cdn-radiotime-logos.tunein.com/s55022q.png",
        )

    def test_manual_tunein_url_is_validated_and_normalized(self):
        self.assertEqual(
            builder.validate_manual_image_url(
                " https://cdn-radiotime-logos.tunein.com/s55022q.png "
            ),
            "http://cdn-radiotime-logos.tunein.com/s55022q.png",
        )

    def test_manual_extensionless_image_url_is_accepted(self):
        self.assertEqual(
            builder.validate_manual_image_url("https://example.com/artwork?id=42"),
            "https://example.com/artwork?id=42",
        )

    def test_manual_jpeg_image_url_is_accepted(self):
        self.assertEqual(
            builder.validate_manual_image_url("https://example.com/artwork.jpg"),
            "https://example.com/artwork.jpg",
        )

    def test_manual_non_http_url_is_rejected(self):
        with self.assertRaises(ValueError):
            builder.validate_manual_image_url("C:/logos/station.png")

    def test_search_query_removes_fusionedge_test_labels(self):
        self.assertEqual(builder.build_search_query("Teszt Tilos (OPUS)"), "Tilos")
        self.assertEqual(builder.build_search_query("BDPST ROCK (Hi-Fi)"), "BDPST ROCK")
        self.assertEqual(builder.build_search_query("Radio BOB! (AC DC)"), "Radio BOB! (AC DC)")

    def test_weak_candidate_is_not_written_automatically(self):
        entry = builder.MapEntry("Unrelated test")
        builder.apply_best_candidate(
            entry,
            [builder.SearchCandidate("Different station", "http://example/logo.png", score=0.2)],
        )
        self.assertEqual(entry.remote_url, "")
        self.assertIn("no reliable match", entry.status)

    def test_specific_name_beats_short_generic_containment(self):
        self.assertGreater(
            builder._candidate_score("Blues Wave", "Blues Wave Radio"),
            builder._candidate_score("Blues Wave", "Blues"),
        )

    def test_generic_one_word_match_does_not_fill_long_station_name(self):
        entry = builder.MapEntry("WDWH-Roots of Rock")
        builder.apply_best_candidate(
            entry,
            [builder.SearchCandidate("Rock", "http://example/logo.png", score=0.84)],
        )
        self.assertEqual(entry.remote_url, "")

    def test_first_reliable_candidate_is_used_after_generic_result(self):
        entry = builder.MapEntry("The Beatles")
        builder.apply_best_candidate(
            entry,
            [
                builder.SearchCandidate("The Beat", "http://example/wrong.png", score=0.94),
                builder.SearchCandidate(
                    "Exclusively The Beatles", "http://example/right.png", score=0.89
                ),
            ],
        )
        self.assertEqual(entry.remote_url, "http://example/right.png")

    def test_word_order_distinguishes_radio_paradise(self):
        self.assertGreater(
            builder._candidate_score("Radio Paradise", "Radio Paradise"),
            builder._candidate_score("Radio Paradise", "Paradise Radio"),
        )

    def test_progressive_jpeg_is_not_decoder_compatible(self):
        progressive = (
            b"\xff\xd8\xff\xc2\x00\x11\x08\x00\x2c\x00\x2c" + b"\x00" * 10
        )
        extension, width, height, compatible = builder.image_info(progressive)
        self.assertEqual((extension, width, height), (".jpg", 44, 44))
        self.assertFalse(compatible)

    def test_selected_jpeg_url_is_kept_and_converted_to_png(self):
        selected = "http://cdn-profiles.tunein.com/s274198/images/logod.jpg?t=153061"
        self.assertEqual(builder.download_candidate_urls(selected)[0], selected)
        with mock.patch.object(builder, "_request_image_bytes", return_value=self.jpeg()):
            logo = builder.download_logo(selected)
        self.assertEqual(logo.source_url, selected)
        self.assertEqual((logo.extension, logo.width, logo.height), (".png", 300, 300))
        with Image.open(io.BytesIO(logo.data)) as image:
            image.load()
            self.assertEqual(image.format, "PNG")

    def test_json_response_falls_back_to_a_valid_image(self):
        selected = "http://cdn-profiles.tunein.com/s274198/images/logod.png"
        fallback = "http://cdn-radiotime-logos.tunein.com/s274198d.png"
        responses = {selected: b'{"error":"not found"}', fallback: self.jpeg()}
        with mock.patch.object(
            builder, "_request_image_bytes", side_effect=lambda url, _timeout: responses[url]
        ):
            logo = builder.download_logo(selected)
        self.assertEqual(logo.source_url, fallback)
        self.assertEqual(logo.extension, ".png")

    def test_non_image_response_is_rejected_even_with_png_suffix(self):
        with mock.patch.object(builder, "_request_image_bytes", return_value=b'{"error":1}'):
            with self.assertRaises(RuntimeError):
                builder.download_logo("http://example.com/logo.png")

    def test_truncated_png_header_is_rejected(self):
        truncated = b"\x89PNG\r\n\x1a\n" + b"\x00\x00\x00\rIHDR" + b"\x00" * 8
        with self.assertRaises(ValueError):
            builder.convert_image_to_png(truncated, "http://example.com/logo.png")

    def test_jpeg_is_resized_to_120_png_with_transparent_padding(self):
        converted = builder.convert_image_to_png(self.jpeg(300, 150), "logo.jpg", 120)
        with Image.open(io.BytesIO(converted.data)) as image:
            image.load()
            self.assertEqual((image.format, image.size), ("PNG", (120, 120)))
            self.assertEqual(image.getpixel((60, 0))[3], 0)
            self.assertEqual(image.getpixel((60, 60))[3], 255)

    def test_map_rejects_non_png_local_filename(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "map.csv"
            with self.assertRaises(ValueError):
                builder.write_map(
                    output,
                    [builder.MapEntry("Station One", local_file="station.jpg")],
                )

    def test_invalid_local_filename_does_not_replace_existing_map(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "map.csv"
            output.write_text("existing map\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                builder.write_map(
                    output,
                    [builder.MapEntry("Station One", local_file="station.jpg")],
                )
            self.assertEqual(output.read_text(encoding="utf-8"), "existing map\n")

    def test_local_filename_is_safe_and_unique(self):
        used = set()
        first = builder.unique_logo_filename("Deja Vu Rádió", ".png", "", used)
        second = builder.unique_logo_filename("Deja Vu Radio", ".png", "", used)
        self.assertEqual(first, "deja-vu-radio.png")
        self.assertEqual(second, "deja-vu-radio-2.png")

    def test_png_is_resized_to_120_square(self):
        source_pixels = bytes((220, 30, 10, 255)) * (300 * 300)
        source = builder.encode_png_rgba(300, 300, source_pixels)
        resized = builder.resize_png(source)
        extension, width, height, compatible = builder.image_info(resized)
        self.assertEqual((extension, width, height, compatible), (".png", 120, 120, True))
        decoded_width, decoded_height, decoded = builder.decode_png_rgba(resized)
        self.assertEqual((decoded_width, decoded_height), (120, 120))
        self.assertEqual(tuple(decoded[:4]), (220, 30, 10, 255))

    def test_non_square_png_is_centered_without_stretching(self):
        source_pixels = bytes((10, 80, 220, 255)) * (300 * 150)
        source = builder.encode_png_rgba(300, 150, source_pixels)
        resized = builder.resize_png(source)
        _width, _height, decoded = builder.decode_png_rgba(resized)
        top_center = (0 * 120 + 60) * 4
        middle_center = (60 * 120 + 60) * 4
        self.assertEqual(decoded[top_center + 3], 0)
        self.assertEqual(tuple(decoded[middle_center : middle_center + 4]), (10, 80, 220, 255))


if __name__ == "__main__":
    unittest.main()
