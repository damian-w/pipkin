"""Exercise release failures that could publish unsafe or misleading flash assets."""

import hashlib
import importlib.util
import json
import shutil
import struct
import tempfile
import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("package_firmware", SOURCE / "scripts/package_firmware.py")
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)
REVISION = "0123456789abcdef0123456789abcdef01234567"


def esp_image(payload):
    header = bytearray(24)
    header[:4] = b"\xe9\x01\x02\x20"
    header[23] = 1
    data = bytes(header) + struct.pack("<II", 0x3F400020, len(payload)) + payload
    checksum = 0xEF
    for byte in payload:
        checksum ^= byte
    data += b"\0" * (15 - len(data) % 16) + bytes([checksum])
    return data + hashlib.sha256(data).digest()


def application(version="1.0.0", revision=REVISION, board="esp32-2432s028r"):
    description = bytearray(256)
    struct.pack_into("<I", description, 0, 0xABCD5432)
    description[16:48] = version.encode().ljust(32, b"\0")
    description[48:80] = b"pipkin".ljust(32, b"\0")
    description[112:144] = b"v5.4.2".ljust(32, b"\0")
    return esp_image(bytes(description) + revision[:12].encode() + b"\0" + board.encode() + b"\0")


def partition_table(nvs_offset=0x9000, flags=0):
    entries = [
        (1, 2, nvs_offset, 0x6000, "nvs", flags),
        (1, 1, 0xF000, 0x1000, "phy_init", 0),
        (0, 0, 0x10000, 0x100000, "factory", 0),
    ]
    data = b"".join(struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size,
                                name.encode().ljust(16, b"\0"), value)
                    for kind, subtype, offset, size, name, value in entries)
    data += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(data).digest()
    return data.ljust(0xC00, b"\xff")


class PackagingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.output = self.root / "release"
        (self.build / "bootloader").mkdir(parents=True)
        (self.build / "partition_table").mkdir()
        (self.build / "bootloader/bootloader.bin").write_bytes(esp_image(b"bootloader"))
        (self.build / "partition_table/partition-table.bin").write_bytes(partition_table())
        (self.build / "pipkin.bin").write_bytes(application())
        sdk = self.root / "sdk"
        runtime = self.root / "toolchain/share/licenses"
        for base, paths in [(sdk, PACKAGER.SDK_NOTICES), (runtime, PACKAGER.RUNTIME_NOTICES)]:
            for name in paths:
                path = base / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f"Copyright fixture: {name}\n")
        xtensa = sdk / "components/xtensa/esp32/include/xtensa/config/core-isa.h"
        xtensa.parent.mkdir(parents=True)
        xtensa.write_text("/* Header description */\n/* Copyright fixture\nPermission is hereby granted */\n")
        self.project = {
            "project_name": "pipkin", "project_version": "1.0.0", "target": "esp32",
            "idf_path": str(sdk), "c_compiler": str(self.root / "toolchain/bin/xtensa-esp32-elf-gcc"),
        }
        self.config = {
            "flash_settings": {"flash_mode": "dio", "flash_freq": "40m", "flash_size": "4MB"},
            "write_flash_args": ["--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB"],
            "extra_esptool_args": {"chip": "esp32"},
            "flash_files": {"0x1000": "bootloader/bootloader.bin", "0x8000": "partition_table/partition-table.bin",
                            "0x10000": "pipkin.bin"},
            "bootloader": {"file": "bootloader/bootloader.bin", "offset": "0x1000", "encrypted": "false"},
            "partition-table": {"file": "partition_table/partition-table.bin", "offset": "0x8000", "encrypted": "false"},
            "app": {"file": "pipkin.bin", "offset": "0x10000", "encrypted": "false"},
        }

    def package(self, tag="v1.0.0", demo=None):
        (self.build / "project_description.json").write_text(json.dumps(self.project))
        (self.build / "flasher_args.json").write_text(json.dumps(self.config))
        return PACKAGER.package(self.build, self.output, SOURCE, REVISION, tag, demo)

    def demo_build(self, version="1.0.0-demo", app_version=None):
        self.package(demo=None)
        shutil.rmtree(self.output)
        demo = self.root / "build-demo"
        shutil.copytree(self.build, demo)
        (demo / "project_description.json").write_text(json.dumps({**self.project, "project_version": version}))
        (demo / "pipkin.bin").write_bytes(application(version=app_version or version))
        return demo

    def rejected(self, message, tag="v1.0.0"):
        with self.assertRaisesRegex(ValueError, message):
            self.package(tag)
        self.assertFalse(self.output.exists(), "validation must complete before release files are written")

    def test_release_contains_separate_images_and_checked_notices(self):
        manifest = self.package()
        self.assertEqual(manifest["source_revision"], REVISION)
        self.assertEqual(manifest["version"], "1.0.0")
        self.assertEqual(manifest["board"], "esp32-2432s028r")
        self.assertEqual(set(manifest), {"product", "version", "source_revision", "board", "images"})
        self.assertEqual([image["offset"] for image in manifest["images"]], [0x1000, 0x8000, 0x10000])
        self.assertEqual([image["role"] for image in manifest["images"]],
                         ["bootloader", "partition-table", "application"])
        self.assertEqual({path.name for path in self.output.iterdir()}, {
            "bootloader.bin", "partition-table.bin", "pipkin.bin", "firmware.json", "SHA256SUMS",
            "LICENSE", "NOTICE.md", "Inter-OFL.txt", "third-party-notices.txt",
        })
        for line in (self.output / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split("  ")
            self.assertEqual(digest, hashlib.sha256((self.output / name).read_bytes()).hexdigest())
        notices = (self.output / "third-party-notices.txt").read_text()
        self.assertIn("gcc/COPYING.RUNTIME", notices)
        self.assertIn("Copyright fixture\nPermission is hereby granted", notices)
        self.assertNotIn(str(self.root), (self.output / "firmware.json").read_text() + notices)
        for image in manifest["images"]:
            data = (self.output / image["file"]).read_bytes()
            self.assertEqual(image["size"], len(data))
            self.assertEqual(image["sha256"], hashlib.sha256(data).hexdigest())

    def test_demo_reel_is_an_extra_checked_asset(self):
        demo = self.demo_build()
        manifest = self.package(demo=demo)
        self.assertEqual([image["file"] for image in manifest["images"]],
                         ["bootloader.bin", "partition-table.bin", "pipkin.bin"])
        self.assertEqual((self.output / "pipkin-demo.bin").read_bytes(), (demo / "pipkin.bin").read_bytes())
        self.assertNotIn("demo", (self.output / "firmware.json").read_text())
        sums = dict(reversed(line.split("  ")) for line in (self.output / "SHA256SUMS").read_text().splitlines())
        self.assertEqual(sums["pipkin-demo.bin"], hashlib.sha256((demo / "pipkin.bin").read_bytes()).hexdigest())

    def test_rejects_a_demo_reel_from_another_version(self):
        for version in ["1.0.1-demo", "1.0.0"]:
            with self.subTest(version=version):
                demo = self.demo_build(version=version)
                with self.assertRaisesRegex(ValueError, "demo: build version"):
                    self.package(demo=demo)
                self.assertFalse(self.output.exists())
                shutil.rmtree(demo)
        demo = self.demo_build(app_version="1.0.0")
        with self.assertRaisesRegex(ValueError, "demo: firmware version"):
            self.package(demo=demo)

    def test_rejects_wrong_tag(self):
        self.rejected("release tag", "v1.0.1")

    def test_rejects_app_version_drift(self):
        (self.build / "pipkin.bin").write_bytes(application(version="1.0.1"))
        self.rejected("firmware version")

    def test_rejects_stale_application(self):
        (self.build / "pipkin.bin").write_bytes(application(revision="f" * 40))
        self.rejected("source revision")

    def test_rejects_stale_compiled_board_identity(self):
        (self.build / "pipkin.bin").write_bytes(application(board="esp32-other-board"))
        self.rejected("compiled board profile")

    def test_rejects_moving_nvs_or_enabling_encryption(self):
        for data in [partition_table(nvs_offset=0xA000), partition_table(flags=1)]:
            with self.subTest(data=data[:32].hex()):
                (self.build / "partition_table/partition-table.bin").write_bytes(data)
                self.rejected("NVS must not move")

    def test_rejects_corrupt_partition_table(self):
        data = bytearray(partition_table())
        data[32] ^= 1
        (self.build / "partition_table/partition-table.bin").write_bytes(data)
        self.rejected("invalid entry")

    def test_rejects_partition_checksum_mismatch(self):
        data = bytearray(partition_table())
        data[112] ^= 1
        (self.build / "partition_table/partition-table.bin").write_bytes(data)
        self.rejected("MD5 checksum")

    def test_rejects_image_corruption_and_truncation(self):
        original = (self.build / "pipkin.bin").read_bytes()
        corrupt = bytearray(original)
        corrupt[300] ^= 1
        for data in [corrupt, original[:-1]]:
            with self.subTest(size=len(data)):
                (self.build / "pipkin.bin").write_bytes(data)
                self.rejected("(checksum|length|digest)")

    def test_rejects_oversized_bootloader(self):
        (self.build / "bootloader/bootloader.bin").write_bytes(b"x" * 0x7001)
        self.rejected("does not fit")

    def test_rejects_flash_offset_drift(self):
        self.config["app"]["offset"] = "0x9000"
        self.rejected("offset differs")

    def test_rejects_unsupported_flash_settings(self):
        self.config["flash_settings"]["flash_size"] = "8MB"
        self.rejected("flash settings")

    def test_rejects_disagreement_between_flashing_arguments(self):
        self.config["write_flash_args"][1] = "qio"
        self.rejected("inconsistent write_flash_args")

    def test_rejects_unlisted_or_extra_images(self):
        self.config["flash_files"]["0x9000"] = "nvs.bin"
        self.rejected("flash_files differs")

    def test_rejects_build_path_escape(self):
        self.config["app"]["file"] = "../pipkin.bin"
        self.rejected("inside the build directory")

    def test_does_not_mix_stale_release_assets(self):
        self.output.mkdir()
        stale = self.output / "old.bin"
        stale.write_bytes(b"old")
        with self.assertRaisesRegex(ValueError, "must be empty"):
            self.package()
        self.assertEqual(stale.read_bytes(), b"old")


class BoardProfileTest(unittest.TestCase):
    def test_reads_the_compiled_board_profile(self):
        self.assertEqual(PACKAGER.board_profile(SOURCE), "esp32-2432s028r")

    def test_rejects_a_missing_or_ambiguous_profile(self):
        with tempfile.TemporaryDirectory() as root:
            header = Path(root) / "main/board.h"
            header.parent.mkdir()
            for text in ["", 'constexpr char kProfile[] = "a";\nconstexpr char kProfile[] = "b";\n']:
                header.write_text(text)
                with self.assertRaisesRegex(ValueError, "one kProfile"):
                    PACKAGER.board_profile(root)


if __name__ == "__main__":
    unittest.main()
