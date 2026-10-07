#!/usr/bin/env python3
"""Package an ESP-IDF build for pipkin flash without filling the NVS gaps."""

import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path


SDK_VERSION = "v5.4.2"
EXPECTED_PARTITIONS = [
    (1, 2, 0x9000, 0x6000, "nvs", 0),
    (1, 1, 0xF000, 0x1000, "phy_init", 0),
    (0, 0, 0x10000, 0x100000, "factory", 0),
]
IMAGE_ROLES = [
    ("bootloader", "bootloader", "bootloader.bin", 0x1000, 0x7000),
    ("partition-table", "partition-table", "partition-table.bin", 0x8000, 0x1000),
    ("application", "app", "pipkin.bin", 0x10000, 0x100000),
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
TLSF_NOTICE = """TLSF allocator: Copyright (c) 2006-2016, 2024 Matthew Conte

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""


def require(condition, message):
    if not condition:
        raise ValueError(message)


def c_string(data):
    return data.split(b"\0", 1)[0].decode("ascii")


def board_profile(source_dir):
    """Resolve the compiled firmware profile from the maintainer's board catalog."""
    source_dir = Path(source_dir)
    catalog = json.loads((source_dir / "boards/profiles.json").read_text())
    require(catalog["schema_version"] == 1 and isinstance(catalog["profiles"], list)
            and catalog["profiles"], "unsupported or empty board profile catalog")
    identifiers = set()
    for profile in catalog["profiles"]:
        require(isinstance(profile["aliases"], list), "board profile aliases must be a list")
        for identifier in [profile["id"], *profile["aliases"]]:
            require(isinstance(identifier, str) and re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", identifier)
                    and identifier not in identifiers, "invalid or duplicate board profile identifier")
            identifiers.add(identifier)
        require(profile["hardware"] in ("unconfirmed", "confirmed"),
                "invalid board profile qualification")
        require(isinstance(profile["variants"], list), "board profile variants must be a list")
        require(profile["hardware"] != "confirmed" or any(
            isinstance(variant, dict) and isinstance(variant.get("qualification"), dict)
            and variant["qualification"].get("display") is True
            and variant["qualification"].get("touch") is True
            for variant in profile["variants"]),
                "confirmed board profile requires recorded hardware evidence")
    header = (source_dir / "main/board.h").read_text()
    constants = {}
    for name in ("Profile", "Hardware"):
        matches = re.findall(r'constexpr char k' + name + r'\[\] = "([^"\n]+)";', header)
        require(len(matches) == 1, f"board header must define one k{name}")
        constants[name] = matches[0]
    selected = [profile for profile in catalog["profiles"] if profile["id"] == constants["Profile"]]
    require(len(selected) == 1, "compiled board profile is not in the catalog")
    profile = selected[0]
    require(profile["hardware"] == constants["Hardware"],
            "board header qualification differs from catalog")
    require((profile["chip"], profile["flash_size"], profile["flash_mode"], profile["flash_freq"],
             profile["layout"]) == ("esp32", 4194304, "dio", "40m", "esp32-single-app-v1"),
            "compiled board profile uses unsupported flash settings")
    require(profile["display"]["controller"] in ("ILI9341", "ILI9341-compatible")
            and (profile["display"]["width"], profile["display"]["height"]) == (320, 240)
            and profile["touch"]["controller"] in ("XPT2046", "XPT2046-compatible")
            and profile["touch"]["type"] == "resistive",
            "compiled board profile uses unsupported display or touch hardware")
    return profile


def validate_image(data, name):
    """Check the ESP32 header, segment lengths, XOR checksum and appended digest."""
    require(len(data) >= 24, f"{name}: truncated ESP image")
    require(data[0] == 0xE9 and 1 <= data[1] <= 16, f"{name}: invalid ESP image header")
    require(data[2] == 2 and data[3] == 0x20, f"{name}: expected DIO, 40 MHz, 4 MB image")
    require(struct.unpack_from("<H", data, 12)[0] == 0, f"{name}: expected ESP32 chip")
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


def validate_application(data, version, revision):
    require(len(data) >= 288, "application: missing ESP-IDF app description")
    require(struct.unpack_from("<I", data, 32)[0] == 0xABCD5432,
            "application: missing ESP-IDF app description")
    require(c_string(data[48:80]) == version, "application: firmware version differs from release tag")
    require(c_string(data[80:112]) == "pipkin", "application: project name is not pipkin")
    require(c_string(data[144:176]) == SDK_VERSION, "application: expected ESP-IDF v5.4.2")
    require(revision[:12].encode("ascii") + b"\0" in data,
            "application: source revision differs from the selected Git commit; rebuild first")


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


def third_party_notices(project):
    sdk = Path(project["idf_path"])
    compiler = Path(project["c_compiler"]).resolve()
    runtime = compiler.parent.parent / "share/licenses"
    sections = ["Pipkin firmware third-party notices\nESP-IDF v5.4.2; Mbed TLS uses its Apache-2.0 option.\n"]
    for root, prefix, names in [(sdk, "ESP-IDF", SDK_NOTICES),
                                (runtime, "Toolchain runtime", RUNTIME_NOTICES)]:
        for name in names:
            text = (root / name).read_text(encoding="utf-8")
            require(text.strip(), f"Missing licence text: {prefix}/{name}")
            sections.append(f"\n===== {prefix}/{name} =====\n\n{text.rstrip()}\n")
    sections.append(f"\n===== TLSF (BSD-3-Clause) =====\n\n{TLSF_NOTICE}")
    # Xtensa's MIT terms and copyright years are in its source headers.
    xtensa = sdk / "components/xtensa/esp32/include/xtensa/config/core-isa.h"
    text = xtensa.read_text(encoding="utf-8")
    start = text.rindex("/*", 0, text.index("Copyright"))
    end = text.index("*/", start) + 2
    require("Permission is hereby granted" in text[start:end], "missing Xtensa MIT licence notice")
    sections.append(f"\n===== ESP32 Xtensa header notice =====\n\n{text[start:end]}\n")
    return "".join(sections).encode("utf-8")


def package(build_dir, output_dir, source_dir, revision, tag=None):
    build_dir = Path(build_dir).resolve()
    output_dir = Path(output_dir).resolve()
    source_dir = Path(source_dir).resolve()
    profile = board_profile(source_dir)
    require(re.fullmatch(r"[0-9a-f]{40}", revision), "source revision must be a full lowercase Git SHA")
    project = json.loads((build_dir / "project_description.json").read_text())
    version = project["project_version"]
    require(re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", version),
            "project version must be a stable semantic version")
    require(tag is None or tag == f"v{version}", "release tag differs from firmware project version")
    require(project["project_name"] == "pipkin" and project["target"] == "esp32",
            "expected a pipkin ESP32 build")
    config = json.loads((build_dir / "flasher_args.json").read_text())
    settings = {"flash_mode": "dio", "flash_freq": "40m", "flash_size": "4MB"}
    require(config["flash_settings"] == settings, "flash settings differ from the initial board profile")
    args = config["write_flash_args"]
    require(len(args) == 6 and {args[i]: args[i + 1] for i in range(0, 6, 2)} ==
            {f"--{key}": value for key, value in settings.items()}, "inconsistent write_flash_args")
    require(config["extra_esptool_args"]["chip"] == "esp32", "flasher chip must be esp32")
    payloads, images, flash_files = {}, [], {}
    for role, key, filename, expected_offset, maximum_size in IMAGE_ROLES:
        description = config[key]
        offset = int(description["offset"], 0)
        require(offset == expected_offset, f"{role}: offset differs from the initial flash layout")
        require(description["encrypted"] in (False, "false"), f"{role}: encrypted image is unsupported")
        path = (build_dir / description["file"]).resolve()
        require(path.is_relative_to(build_dir), f"{role}: image must be inside the build directory")
        require(path.name == filename, f"{role}: unexpected image filename")
        flash_files[offset] = description["file"]
        data = path.read_bytes()
        require(0 < len(data) <= maximum_size, f"{role}: image does not fit its flash region")
        if role == "partition-table":
            validate_partitions(data)
        else:
            validate_image(data, role)
        if role == "application":
            validate_application(data, version, revision)
            require(profile["id"].encode("ascii") + b"\0" in data,
                    "application: compiled board profile differs from the catalog; rebuild first")
        payloads[filename] = data
        images.append({"role": role, "file": filename, "offset": offset,
                       "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    require({int(offset, 0): filename for offset, filename in config["flash_files"].items()} == flash_files,
            "flash_files differs from the selected three images")
    manifest = {
        "schema_version": 1, "product": "pipkin", "version": version,
        "source_revision": revision, "board": profile["id"],
        "hardware": profile["hardware"], "chip": profile["chip"], "flash_size": profile["flash_size"],
        "flash_mode": settings["flash_mode"], "flash_freq": settings["flash_freq"],
        "protocol_min": 1, "protocol_max": 1, "minimum_cli": "1.4.0",
        "layout": profile["layout"], "images": images,
    }
    payloads["firmware.json"] = (json.dumps(manifest, indent=2) + "\n").encode("utf-8")
    payloads["LICENSE"] = (source_dir / "LICENSE").read_bytes()
    payloads["NOTICE.md"] = (source_dir / "assets/NOTICE.md").read_text().replace(
        "[LICENSE](../LICENSE)", "[LICENSE](LICENSE)").encode("utf-8")
    payloads["Inter-OFL.txt"] = (source_dir / "assets/Inter-OFL.txt").read_bytes()
    payloads["third-party-notices.txt"] = third_party_notices(project)
    payloads["SHA256SUMS"] = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n"
        for name, data in sorted(payloads.items())).encode("ascii")
    require(not output_dir.exists() or not any(output_dir.iterdir()), "output directory must be empty")
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, data in payloads.items():
        (output_dir / name).write_bytes(data)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--source-revision", help="Full Git commit SHA; defaults to source HEAD")
    parser.add_argument("--tag", help="Release tag, which must equal v<firmware version>")
    args = parser.parse_args()
    try:
        revision = args.source_revision or subprocess.check_output(
            ["git", "-C", str(args.source_dir), "rev-parse", "HEAD"], text=True).strip()
        manifest = package(args.build_dir, args.output_dir, args.source_dir, revision, args.tag)
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Cannot package firmware: {error}\n")
    print(f"Packaged Pipkin {manifest['version']} ({manifest['source_revision'][:12]}) in {args.output_dir}")


if __name__ == "__main__":
    main()
