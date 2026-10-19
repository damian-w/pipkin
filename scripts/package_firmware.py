#!/usr/bin/env python3
"""Package ESP-IDF builds for pipkin flash without filling the NVS gaps.

Each build's target selects its board. The 2.8-inch CYD (esp32) keeps the original asset
names, firmware.json and its images, which every 2.x CLI reads; other boards add their own
manifest and suffixed images, which CLIs from before that board ignore.
"""

import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path


SDK_VERSION = "v5.4.2"
FLASH_SETTINGS = {"flash_mode": "dio", "flash_freq": "40m", "flash_size": "4MB"}
EXPECTED_PARTITIONS = [
    (1, 2, 0x9000, 0x6000, "nvs", 0),
    (1, 1, 0xF000, 0x1000, "phy_init", 0),
    (0, 0, 0x10000, 0x100000, "factory", 0),
]
# Per ESP-IDF target: board profile, image header chip ID, bootloader offset and maximum size,
# and the suffix of its release asset names.
BOARDS = {
    "esp32": {"board": "esp32-2432s028r", "chip_id": 0, "bootloader": (0x1000, 0x7000), "suffix": ""},
    "esp32s3": {"board": "es3c40p", "chip_id": 9, "bootloader": (0x0, 0x8000), "suffix": "-es3c40p"},
}


def image_roles(board):
    """(role, flasher_args key, build file name, release file name, offset, maximum size)."""
    offset, maximum = board["bootloader"]
    suffix = board["suffix"]
    return [
        ("bootloader", "bootloader", "bootloader.bin", f"bootloader{suffix}.bin", offset, maximum),
        ("partition-table", "partition-table", "partition-table.bin", f"partition-table{suffix}.bin",
         0x8000, 0x1000),
        ("application", "app", "pipkin.bin", f"pipkin{suffix}.bin", 0x10000, 0x100000),
    ]


SDK_NOTICES = [
    "LICENSE",
    "docs/en/COPYRIGHT.rst",
    "components/freertos/FreeRTOS-Kernel/LICENSE.md",
    "components/newlib/COPYING.NEWLIB",
    "components/mbedtls/mbedtls/LICENSE",
]
RUNTIME_NOTICES = [
    "gcc/COPYING3",
    "gcc/COPYING3.LIB",
    "gcc/COPYING.RUNTIME",
    "newlib/COPYING.NEWLIB",
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def c_string(data):
    return data.split(b"\0", 1)[0].decode("ascii")


def validate_image(data, name, chip_id):
    """Check the ESP header, segment lengths, XOR checksum and appended digest."""
    require(len(data) >= 24, f"{name}: truncated ESP image")
    require(data[0] == 0xE9 and 1 <= data[1] <= 16, f"{name}: invalid ESP image header")
    require(data[2] == 2 and data[3] == 0x20, f"{name}: expected DIO, 40 MHz, 4 MB image")
    require(struct.unpack_from("<H", data, 12)[0] == chip_id, f"{name}: image is for another chip")
    require(data[23] == 1, f"{name}: appended SHA-256 required")
    cursor, checksum = 24, 0xEF
    for _ in range(data[1]):
        require(cursor + 8 <= len(data), f"{name}: truncated segment header")
        length = struct.unpack_from("<I", data, cursor + 4)[0]
        cursor += 8
        require(cursor + length <= len(data), f"{name}: truncated image segment")
        for byte in data[cursor:cursor + length]:
            checksum ^= byte
        cursor += length
    checksum_offset = ((cursor + 16) // 16) * 16 - 1
    require(len(data) == checksum_offset + 1 + 32, f"{name}: invalid image length")
    require(data[checksum_offset] == checksum, f"{name}: image checksum mismatch")
    require(hashlib.sha256(data[:-32]).digest() == data[-32:], f"{name}: image digest mismatch")


def validate_application(data, version, revision, board, name="application"):
    require(len(data) >= 288, f"{name}: missing ESP-IDF app description")
    require(struct.unpack_from("<I", data, 32)[0] == 0xABCD5432,
            f"{name}: missing ESP-IDF app description")
    require(c_string(data[48:80]) == version, f"{name}: firmware version differs from release tag")
    require(c_string(data[80:112]) == "pipkin", f"{name}: project name is not pipkin")
    require(c_string(data[144:176]) == SDK_VERSION, f"{name}: expected ESP-IDF {SDK_VERSION}")
    require(revision[:12].encode("ascii") + b"\0" in data,
            f"{name}: source revision differs from the selected Git commit; rebuild first")
    require(board.encode("ascii") + b"\0" in data,
            f"{name}: compiled board profile differs from main/board.h; rebuild first")


def demo_application(build_dir, version, revision):
    """A demo reel build (-DPIPKIN_DEMO=ON), which pipkin flash --demo writes in place of the
    board's application. It is versioned <version>-demo and stays out of the manifests.
    Returns its target and image."""
    build_dir = Path(build_dir).resolve()
    project = json.loads((build_dir / "project_description.json").read_text())
    target = project["target"]
    require(project["project_name"] == "pipkin" and target in BOARDS,
            "demo: expected a pipkin build for a supported board")
    require(project["project_version"] == f"{version}-demo", "demo: build version differs from the release")
    data = (build_dir / "pipkin.bin").read_bytes()
    require(0 < len(data) <= 0x100000, "demo: image does not fit its flash region")
    board = BOARDS[target]
    validate_image(data, "demo", board["chip_id"])
    validate_application(data, f"{version}-demo", revision, board["board"], "demo")
    return target, data


def validate_partitions(data):
    require(len(data) == 0xC00, "partition table: expected an unsigned 3072-byte table")
    partitions = []
    checksum_seen = False
    for offset in range(0, len(data), 32):
        entry = data[offset:offset + 32]
        if entry == b"\xff" * 32:
            require(checksum_seen and data[offset:] == b"\xff" * (len(data) - offset),
                    "partition table: missing checksum or invalid padding")
            break
        if entry[:16] == b"\xeb\xeb" + b"\xff" * 14:
            require(not checksum_seen and entry[16:] == hashlib.md5(data[:offset]).digest(),
                    "partition table: MD5 checksum mismatch")
            checksum_seen = True
            continue
        require(not checksum_seen, "partition table: entry after checksum")
        magic, kind, subtype, start, size, label, flags = struct.unpack("<HBBII16sI", entry)
        require(magic == 0x50AA, "partition table: invalid entry")
        partitions.append((kind, subtype, start, size, c_string(label), flags))
    else:
        raise ValueError("partition table: missing end marker")
    require(partitions == EXPECTED_PARTITIONS,
            "partition table: does not match esp32-single-app-v1; NVS must not move")


def third_party_notices(project, source_dir, targets):
    sdk = Path(project["idf_path"])
    compiler = Path(project["c_compiler"]).resolve()
    runtime = compiler.parent.parent / "share/licenses"
    sections = [f"Pipkin firmware third-party notices\nESP-IDF {SDK_VERSION}; Mbed TLS uses its Apache-2.0 option.\n"]
    for root, prefix, names in [(sdk, "ESP-IDF", SDK_NOTICES),
                                (runtime, "Toolchain runtime", RUNTIME_NOTICES)]:
        for name in names:
            text = (root / name).read_text(encoding="utf-8")
            require(text.strip(), f"Missing licence text: {prefix}/{name}")
            sections.append(f"\n===== {prefix}/{name} =====\n\n{text.rstrip()}\n")
    tlsf = (source_dir / "assets/TLSF-BSD-3-Clause.txt").read_text(encoding="utf-8")
    sections.append(f"\n===== TLSF (BSD-3-Clause) =====\n\n{tlsf}")
    # Xtensa's MIT terms and copyright years are in each chip's source headers.
    for target in targets:
        xtensa = sdk / f"components/xtensa/{target}/include/xtensa/config/core-isa.h"
        text = xtensa.read_text(encoding="utf-8")
        start = text.rindex("/*", 0, text.index("Copyright"))
        end = text.index("*/", start) + 2
        require("Permission is hereby granted" in text[start:end], "missing Xtensa MIT licence notice")
        name = target.upper().replace("ESP32S3", "ESP32-S3")
        sections.append(f"\n===== {name} Xtensa header notice =====\n\n{text[start:end]}\n")
    return "".join(sections).encode("utf-8")


def board_build(build_dir, revision, tag):
    """Validate one board's build; returns its project description, manifest and release payloads."""
    build_dir = Path(build_dir).resolve()
    project = json.loads((build_dir / "project_description.json").read_text())
    version = project["project_version"]
    require(re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", version),
            "project version must be a stable semantic version")
    require(tag is None or tag == f"v{version}", "release tag differs from firmware project version")
    target = project["target"]
    require(project["project_name"] == "pipkin" and target in BOARDS, "expected a pipkin build for a supported board")
    board = BOARDS[target]
    config = json.loads((build_dir / "flasher_args.json").read_text())
    require(config["flash_settings"] == FLASH_SETTINGS, "flash settings differ from DIO, 40 MHz and 4 MB")
    payloads, images, flash_files = {}, [], {}
    for role, key, filename, release_name, expected_offset, maximum_size in image_roles(board):
        description = config[key]
        offset = int(description["offset"], 0)
        require(offset == expected_offset, f"{role}: offset differs from the initial flash layout")
        require(description["encrypted"] == "false", f"{role}: encrypted image is unsupported")
        path = (build_dir / description["file"]).resolve()
        require(path.is_relative_to(build_dir), f"{role}: image must be inside the build directory")
        require(path.name == filename, f"{role}: unexpected image filename")
        flash_files[offset] = description["file"]
        data = path.read_bytes()
        require(0 < len(data) <= maximum_size, f"{role}: image does not fit its flash region")
        if role == "partition-table":
            validate_partitions(data)
        else:
            validate_image(data, role, board["chip_id"])
        if role == "application":
            validate_application(data, version, revision, board["board"])
        payloads[release_name] = data
        images.append({"role": role, "file": release_name, "offset": offset,
                       "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    require({int(offset, 0): filename for offset, filename in config["flash_files"].items()} == flash_files,
            "flash_files differs from the selected three images")
    manifest = {"product": "pipkin", "version": version, "source_revision": revision,
                "board": board["board"], "images": images}
    payloads[f"firmware{board['suffix']}.json"] = (json.dumps(manifest, indent=2) + "\n").encode("utf-8")
    return project, manifest, payloads


def package(build_dirs, output_dir, source_dir, revision, tag=None, demo_build_dirs=()):
    """Package one build per board, the CYD's first. Returns the CYD's manifest."""
    output_dir = Path(output_dir).resolve()
    source_dir = Path(source_dir).resolve()
    require(re.fullmatch(r"[0-9a-f]{40}", revision), "source revision must be a full lowercase Git SHA")
    builds = [board_build(path, revision, tag) for path in build_dirs]
    targets = [project["target"] for project, _, _ in builds]
    require(targets and targets[0] == "esp32", "the first build must be the esp32 CYD, which firmware.json describes")
    require(len(set(targets)) == len(targets), "each board needs exactly one build")
    versions = {manifest["version"] for _, manifest, _ in builds}
    require(len(versions) == 1, "board builds have different firmware versions")
    version = versions.pop()
    payloads = {}
    for _, _, board_payloads in builds:
        payloads.update(board_payloads)
    for path in demo_build_dirs:
        target, data = demo_application(path, version, revision)
        demo_name = f"pipkin-demo{BOARDS[target]['suffix']}.bin"
        require(target in targets and demo_name not in payloads, "each demo reel needs its board's build, once")
        payloads[demo_name] = data
    payloads["LICENSE"] = (source_dir / "LICENSE").read_bytes()
    payloads["NOTICE.md"] = (source_dir / "assets/NOTICE.md").read_text().replace(
        "[LICENSE](../LICENSE)", "[LICENSE](LICENSE)").encode("utf-8")
    payloads["Inter-OFL.txt"] = (source_dir / "assets/Inter-OFL.txt").read_bytes()
    payloads["third-party-notices.txt"] = third_party_notices(builds[0][0], source_dir, targets)
    payloads["SHA256SUMS"] = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n"
        for name, data in sorted(payloads.items())).encode("ascii")
    require(not output_dir.exists() or not any(output_dir.iterdir()), "output directory must be empty")
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, data in payloads.items():
        (output_dir / name).write_bytes(data)
    return builds[0][1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, action="append",
                        help="A board's build, the esp32 CYD first; repeat for each board (default: build)")
    parser.add_argument("--demo-build-dir", type=Path, action="append", default=[],
                        help="Also package this demo reel build; repeat for each board")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--source-revision", help="Full Git commit SHA; defaults to source HEAD")
    parser.add_argument("--tag", help="Release tag, which must equal v<firmware version>")
    args = parser.parse_args()
    try:
        revision = args.source_revision or subprocess.check_output(
            ["git", "-C", str(args.source_dir), "rev-parse", "HEAD"], text=True).strip()
        manifest = package(args.build_dir or [Path("build")], args.output_dir, args.source_dir, revision,
                           args.tag, args.demo_build_dir)
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Cannot package firmware: {error}\n")
    print(f"Packaged Pipkin {manifest['version']} ({manifest['source_revision'][:12]}) in {args.output_dir}")


if __name__ == "__main__":
    main()
