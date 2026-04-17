#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import zipfile
from datetime import datetime, timezone
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Package PlatformIO firmware outputs.")
    parser.add_argument("--env", required=True, help="PlatformIO environment name")
    parser.add_argument(
        "--version",
        default="dev",
        help="Version or tag name to embed in the package metadata",
    )
    parser.add_argument(
        "--project",
        default="DraARL-ESP32-RF",
        help="Project name used in the package file name",
    )
    parser.add_argument(
        "--output-dir",
        default="dist",
        help="Directory where packaged outputs will be written",
    )
    return parser.parse_args()


def sanitize_component(value: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9._-]+", "-", value.strip())
    sanitized = sanitized.strip(".-")
    return sanitized or "package"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def find_boot_app0(repo_root: Path) -> Path | None:
    candidates = []

    platformio_core_dir = os.environ.get("PLATFORMIO_CORE_DIR")
    if platformio_core_dir:
        candidates.append(Path(platformio_core_dir))

    candidates.extend(
        [
            repo_root / ".platformio",
            Path.home() / ".platformio",
        ]
    )

    for base_dir in candidates:
        candidate = base_dir / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"
        if candidate.exists():
            return candidate

    return None


def build_flash_entries(repo_root: Path, build_dir: Path) -> list[dict]:
    required_images = [
        ("0x0000", build_dir / "bootloader.bin"),
        ("0x8000", build_dir / "partitions.bin"),
        ("0x10000", build_dir / "firmware.bin"),
    ]
    entries: list[dict] = []

    for offset, source in required_images:
        if not source.exists():
            raise FileNotFoundError(f"Missing build artifact: {source}")

        entries.append(
            {
                "offset": offset,
                "source": source,
                "packaged_name": source.name,
            }
        )

    boot_app0_path = find_boot_app0(repo_root)
    if boot_app0_path is not None:
        entries.insert(
            2,
            {
                "offset": "0xe000",
                "source": boot_app0_path,
                "packaged_name": boot_app0_path.name,
            },
        )

    return entries


def write_readme(path: Path, project: str, version: str, env_name: str, flash_entries: list[dict]) -> None:
    lines = [
        f"{project} firmware package",
        f"Version: {version}",
        f"Environment: {env_name}",
        "",
        "Included flash images:",
    ]

    for entry in flash_entries:
        lines.append(f"  {entry['offset']}  {entry['packaged_name']}")

    lines.extend(
        [
            "",
            "Example with esptool.py:",
            "  esptool.py --chip esp32s3 write_flash -z \\",
        ]
    )

    for index, entry in enumerate(flash_entries):
        suffix = " \\" if index < len(flash_entries) - 1 else ""
        lines.append(f"    {entry['offset']} {entry['packaged_name']}{suffix}")

    lines.extend(
        [
            "",
            "You can also flash from PlatformIO, or use the files above with your preferred ESP32 flashing tool.",
        ]
    )

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_flash_args(path: Path, flash_entries: list[dict]) -> None:
    content = "\n".join(f"{entry['offset']} {entry['packaged_name']}" for entry in flash_entries) + "\n"
    path.write_text(content, encoding="utf-8")


def write_sha256_sums(path: Path, files: list[Path]) -> None:
    lines = [f"{sha256_file(file_path)}  {file_path.name}" for file_path in files]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def zip_directory(source_dir: Path, zip_path: Path) -> None:
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for file_path in sorted(source_dir.rglob("*")):
            if file_path.is_file():
                archive.write(file_path, file_path.relative_to(source_dir))


def main() -> int:
    args = parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    build_dir = repo_root / ".pio" / "build" / args.env
    flash_entries = build_flash_entries(repo_root, build_dir)

    package_base = "-".join(
        [
            sanitize_component(args.project),
            sanitize_component(args.version),
            sanitize_component(args.env),
        ]
    )
    output_root = repo_root / args.output_dir
    package_dir = output_root / package_base
    zip_path = output_root / f"{package_base}.zip"

    if package_dir.exists():
        shutil.rmtree(package_dir)
    package_dir.mkdir(parents=True, exist_ok=True)
    output_root.mkdir(parents=True, exist_ok=True)

    packaged_files: list[Path] = []
    manifest_entries: list[dict] = []
    for entry in flash_entries:
        destination = package_dir / entry["packaged_name"]
        shutil.copy2(entry["source"], destination)
        packaged_files.append(destination)
        manifest_entries.append(
            {
                "offset": entry["offset"],
                "file": entry["packaged_name"],
                "sha256": sha256_file(destination),
                "size": destination.stat().st_size,
            }
        )

    readme_path = package_dir / "README.txt"
    flash_args_path = package_dir / "flash_args.txt"
    manifest_path = package_dir / "flash_manifest.json"
    sha256_path = package_dir / "SHA256SUMS.txt"

    write_readme(readme_path, args.project, args.version, args.env, flash_entries)
    write_flash_args(flash_args_path, flash_entries)

    manifest = {
        "project": args.project,
        "version": args.version,
        "environment": args.env,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "images": manifest_entries,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    write_sha256_sums(sha256_path, packaged_files)

    if zip_path.exists():
        zip_path.unlink()
    zip_directory(package_dir, zip_path)

    print(f"Package directory: {package_dir}")
    print(f"Package zip: {zip_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
