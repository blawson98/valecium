#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import shlex

SECTOR_SIZE = 512
STAGE2_LOAD_ADDR = 0x7E00
COREFS_LOAD_ADDR = 0x57E00
DEFAULT_LABEL = "VALECIUM"


class DiskBuildError(RuntimeError):
    pass


def run_command(args, input_text=None, **kwargs):
    subprocess.run(
        args,
        check=True,
        input=input_text,
        text=input_text is not None,
        **kwargs,
    )


def require_tool(name):
    if shutil.which(name) is None:
        raise DiskBuildError(f"Missing required tool: {name}")


def parse_size(value):
    value = value.strip()
    if not value:
        raise DiskBuildError("Image size is empty")

    suffix = value[-1].upper()
    if suffix in ("K", "M", "G", "T"):
        number = value[:-1]
        if not number:
            raise DiskBuildError(f"Invalid size: {value}")
        multiplier = {
            "K": 1024,
            "M": 1024 * 1024,
            "G": 1024 * 1024 * 1024,
            "T": 1024 * 1024 * 1024 * 1024,
        }[suffix]
        return int(number) * multiplier

    return int(value)


def align_up(value, align):
    return (value + align - 1) // align * align


def get_dir_size(path):
    total = 0
    for root, _, files in os.walk(path):
        for name in files:
            full = os.path.join(root, name)
            try:
                total += os.lstat(full).st_size
            except FileNotFoundError:
                continue
    return total


def patch_stage1(stage1_bytes, stage2_lba, stage2_sectors):
    sig = b"VLSP"
    idx = stage1_bytes.find(sig)
    if idx == -1:
        raise DiskBuildError("Stage1 signature VLSP not found")
    if stage1_bytes.find(sig, idx + 1) != -1:
        raise DiskBuildError("Stage1 signature VLSP appears multiple times")

    data = bytearray(stage1_bytes)
    struct.pack_into("<I", data, idx + 4, stage2_lba)
    struct.pack_into("<H", data, idx + 8, stage2_sectors)
    return bytes(data)


def patch_stage2(stage2_bytes, label_bytes, uuid_bytes):
    sig = b"VLSF"
    idx = stage2_bytes.find(sig)
    if idx == -1:
        raise DiskBuildError("Stage2 signature VLSF not found")
    if stage2_bytes.find(sig, idx + 1) != -1:
        raise DiskBuildError("Stage2 signature VLSF appears multiple times")

    data = bytearray(stage2_bytes)
    struct.pack_into("<I", data, idx + 4, COREFS_LOAD_ADDR)
    data[idx + 8 : idx + 8 + 32] = label_bytes
    data[idx + 40 : idx + 40 + 16] = uuid_bytes
    return bytes(data)


def build_label_bytes(label):
    if not label:
        return b"\x00" * 32
    encoded = label.encode("ascii", errors="replace")
    return encoded[:32].ljust(32, b"\x00")


def parse_uuid_bytes(uuid_str):
    if not uuid_str:
        return None
    compact = uuid_str.replace("-", "").strip()
    if len(compact) != 32:
        return None
    try:
        return bytes.fromhex(compact)
    except ValueError:
        return None


def build_stage2_blob(stage2_bytes, corefs_bytes):
    if corefs_bytes is None:
        combined = stage2_bytes
    else:
        corefs_offset = COREFS_LOAD_ADDR - STAGE2_LOAD_ADDR
        if corefs_offset < len(stage2_bytes):
            raise DiskBuildError(
                "Stage2 is larger than the corefs load offset; refuse to append corefs."
            )
        padding = corefs_offset - len(stage2_bytes)
        combined = stage2_bytes + (b"\x00" * padding) + corefs_bytes

    sectors = (len(combined) + SECTOR_SIZE - 1) // SECTOR_SIZE
    total = sectors * SECTOR_SIZE
    combined = combined.ljust(total, b"\x00")
    return combined, sectors


def write_at(path, offset, data):
    with open(path, "r+b") as handle:
        handle.seek(offset)
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())


def copy_tree_contents(src, dst):
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        target_root = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(target_root, exist_ok=True)
        shutil.copystat(root, target_root, follow_symlinks=False)
        for name in dirs:
            src_path = os.path.join(root, name)
            dst_path = os.path.join(target_root, name)
            if os.path.islink(src_path):
                if os.path.lexists(dst_path):
                    os.remove(dst_path)
                os.symlink(os.readlink(src_path), dst_path)
            else:
                os.makedirs(dst_path, exist_ok=True)
                shutil.copystat(src_path, dst_path, follow_symlinks=False)
        for name in files:
            src_path = os.path.join(root, name)
            dst_path = os.path.join(target_root, name)
            if os.path.islink(src_path):
                if os.path.lexists(dst_path):
                    os.remove(dst_path)
                os.symlink(os.readlink(src_path), dst_path)
            else:
                shutil.copy2(src_path, dst_path, follow_symlinks=False)


def wait_for_partition(loop_dev):
    candidates = [f"{loop_dev}p1", f"{loop_dev}1"]
    for _ in range(50):
        for candidate in candidates:
            if os.path.exists(candidate):
                return candidate
        run_command(["udevadm", "settle"], stdout=subprocess.DEVNULL)
        time.sleep(0.1)
    raise DiskBuildError("Partition device did not appear for loopback")


def partition_mbr(image_path, start_lba, total_sectors):
    end_lba = total_sectors - 1
    if end_lba <= start_lba:
        raise DiskBuildError("Partition size is invalid")

    run_command(["parted", "-s", image_path, "mklabel", "msdos"])
    run_command(
        [
            "parted",
            "-s",
            "-a",
            "minimal",
            image_path,
            "mkpart",
            "primary",
            f"{start_lba}s",
            f"{end_lba}s",
        ]
    )
    run_command(["parted", "-s", image_path, "set", "1", "boot", "on"])


def partition_gpt(image_path, start_lba, total_sectors, label):
    if not shutil.which("parted"):
        raise DiskBuildError("GPT requires parted")

    end_lba = total_sectors - 34
    if end_lba <= start_lba:
        raise DiskBuildError("Image size is too small for GPT partition")

    run_command(["parted", "-s", image_path, "mklabel", "gpt"])
    run_command(
        [
            "parted",
            "-s",
            "-a",
            "minimal",
            image_path,
            "mkpart",
            "primary",
            f"{start_lba}s",
            f"{end_lba}s",
        ]
    )
    run_command(["parted", "-s", image_path, "name", "1", label])


def mkfs_label_args(fs_type, label):
    if not label:
        return []
    mapping = {
        "ext2": ["-L", label],
        "ext3": ["-L", label],
        "ext4": ["-L", label],
        "xfs": ["-L", label],
        "btrfs": ["-L", label],
        "vfat": ["-n", label],
        "fat": ["-n", label],
        "msdos": ["-n", label],
        "exfat": ["-n", label],
        "f2fs": ["-l", label],
    }
    return mapping.get(fs_type, [])


def read_blkid(part_dev):
    result = subprocess.run(
        ["blkid", "-o", "export", part_dev],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if result.returncode != 0:
        return None, None
    label = None
    uuid = None
    for line in result.stdout.splitlines():
        if line.startswith("LABEL="):
            label = line.split("=", 1)[1].strip()
        elif line.startswith("UUID="):
            uuid = line.split("=", 1)[1].strip()
    return label, uuid


def main():
    parser = argparse.ArgumentParser(
        description="Build a bootable disk image for the Valecium bootloader."
    )
    parser.add_argument("--stage1", required=True, help="Path to stage1 boot.bin")
    parser.add_argument("--stage2", required=True, help="Path to stage2 core.bin")
    parser.add_argument(
        "--staging",
        required=True,
        help="Directory containing files to copy into the image",
    )
    parser.add_argument("--output", required=True, help="Output disk image path")
    parser.add_argument("--fs", required=True, help="Filesystem type (mkfs.<fs>)")
    parser.add_argument(
        "--scheme",
        required=True,
        choices=["mbr", "gpt"],
        help="Partition scheme",
    )
    parser.add_argument("--label", default=DEFAULT_LABEL, help="Filesystem label")
    parser.add_argument("--image-size", help="Image size (bytes or with K/M/G)")
    parser.add_argument(
        "--partition-start",
        type=int,
        default=2048,
        help="Partition start LBA (default: 2048)",
    )
    parser.add_argument(
        "--stage2-start",
        type=int,
        help="Stage2 start LBA (defaults to 1 for MBR, 34 for GPT)",
    )
    parser.add_argument(
        "--corefs",
        help="Optional corefs_<fs>.bin path (default: auto-detect)",
    )
    parser.add_argument(
        "--mkfs-args",
        default="",
        help="Extra arguments to pass to mkfs.<fs>",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite existing output image",
    )

    args = parser.parse_args()

    if not sys.platform.startswith("linux"):
        raise DiskBuildError("This script only supports Linux")
    if os.geteuid() != 0:
        raise DiskBuildError("Run this script as root (sudo)")

    stage1_path = os.path.abspath(args.stage1)
    stage2_path = os.path.abspath(args.stage2)
    staging_dir = os.path.abspath(args.staging)
    output_path = os.path.abspath(args.output)

    if not os.path.isfile(stage1_path):
        raise DiskBuildError(f"Stage1 not found: {stage1_path}")
    if not os.path.isfile(stage2_path):
        raise DiskBuildError(f"Stage2 not found: {stage2_path}")
    if not os.path.isdir(staging_dir):
        raise DiskBuildError(f"Staging dir not found: {staging_dir}")

    require_tool("losetup")
    require_tool("parted")
    require_tool("mount")
    require_tool("umount")
    require_tool("blkid")
    require_tool("udevadm")
    require_tool("partprobe")
    require_tool(f"mkfs.{args.fs}")

    if args.scheme == "gpt":
        if not shutil.which("parted"):
            raise DiskBuildError("GPT requires parted")

    if os.path.exists(output_path):
        if args.force:
            os.remove(output_path)
        else:
            raise DiskBuildError(f"Output already exists: {output_path} (use --force)")

    with open(stage1_path, "rb") as handle:
        stage1_bytes = handle.read()
    with open(stage2_path, "rb") as handle:
        stage2_base = handle.read()

    stage2_start = args.stage2_start
    if stage2_start is None:
        stage2_start = 1 if args.scheme == "mbr" else 34

    if stage2_start <= 0:
        raise DiskBuildError("Stage2 start LBA must be >= 1")
    if args.scheme == "gpt" and stage2_start < 34:
        raise DiskBuildError("GPT stage2 start must be >= 34")

    corefs_bytes = None
    corefs_path = None
    if args.corefs:
        corefs_path = os.path.abspath(args.corefs)
        if not os.path.isfile(corefs_path):
            raise DiskBuildError(f"Corefs not found: {corefs_path}")
        with open(corefs_path, "rb") as handle:
            corefs_bytes = handle.read()
    else:
        guessed = os.path.join(os.path.dirname(stage2_path), f"corefs_{args.fs}.bin")
        if os.path.isfile(guessed):
            corefs_path = guessed
            with open(corefs_path, "rb") as handle:
                corefs_bytes = handle.read()
        else:
            print(
                f"Warning: corefs_{args.fs}.bin not found; stage2 will not "
                "include a filesystem driver.",
                file=sys.stderr,
            )

    if corefs_bytes is not None:
        corefs_offset = COREFS_LOAD_ADDR - STAGE2_LOAD_ADDR
        if len(stage2_base) >= corefs_offset:
            print(
                "Warning: Stage2 is already at/after the corefs offset; "
                "skipping corefs append.",
                file=sys.stderr,
            )
            corefs_bytes = None

    _, stage2_sectors = build_stage2_blob(stage2_base, corefs_bytes)

    if stage2_start + stage2_sectors > args.partition_start:
        raise DiskBuildError(
            "Stage2 overlaps the main partition; adjust start LBAs or size"
        )

    staging_size = get_dir_size(staging_dir)
    min_size = args.partition_start * SECTOR_SIZE + staging_size + 64 * 1024 * 1024
    if args.image_size:
        image_size = parse_size(args.image_size)
        if image_size < min_size:
            raise DiskBuildError("Image size is too small for staging content")
    else:
        image_size = align_up(min_size, 1024 * 1024)

    total_sectors = image_size // SECTOR_SIZE
    if total_sectors <= args.partition_start:
        raise DiskBuildError("Image size does not allow a partition")

    with open(output_path, "wb") as handle:
        handle.truncate(image_size)

    if args.scheme == "mbr":
        partition_mbr(output_path, args.partition_start, total_sectors)
    else:
        partition_gpt(output_path, args.partition_start, total_sectors, args.label)

    stage1_patched = patch_stage1(stage1_bytes, stage2_start, stage2_sectors)
    if len(stage1_patched) > 0x1BE:
        raise DiskBuildError(f"Stage1 size exceeds 0x1BE bytes: {len(stage1_patched)}")
    stage1_patched = stage1_patched.ljust(0x1BE, b"\x00")
    write_at(output_path, 0, stage1_patched)

    loop_dev = None
    mount_dir = None
    part_dev = None

    try:
        loop_dev = subprocess.check_output(
            ["losetup", "--find", "--show", "--partscan", output_path],
            text=True,
        ).strip()
        run_command(["partprobe", loop_dev], stdout=subprocess.DEVNULL)
        part_dev = wait_for_partition(loop_dev)

        mkfs_cmd = [f"mkfs.{args.fs}"]
        mkfs_cmd.extend(mkfs_label_args(args.fs, args.label))
        if args.mkfs_args:
            mkfs_cmd.extend(shlex.split(args.mkfs_args))
        mkfs_cmd.append(part_dev)
        run_command(mkfs_cmd)

        fs_label, fs_uuid = read_blkid(part_dev)
        if not fs_label:
            fs_label = args.label
        uuid_bytes = parse_uuid_bytes(fs_uuid)
        if uuid_bytes is None:
            uuid_bytes = b"\x00" * 16

        label_bytes = build_label_bytes(fs_label)
        stage2_patched = patch_stage2(stage2_base, label_bytes, uuid_bytes)
        stage2_blob, _ = build_stage2_blob(stage2_patched, corefs_bytes)
        write_at(output_path, stage2_start * SECTOR_SIZE, stage2_blob)

        mount_dir = tempfile.mkdtemp(prefix="valecium-img-")
        run_command(["mount", part_dev, mount_dir])
        copy_tree_contents(staging_dir, mount_dir)
        run_command(["sync"])
        run_command(["umount", mount_dir])
        shutil.rmtree(mount_dir, ignore_errors=True)
        mount_dir = None
    finally:
        if mount_dir:
            try:
                run_command(["umount", mount_dir])
            except Exception:
                pass
            shutil.rmtree(mount_dir, ignore_errors=True)
        if loop_dev:
            try:
                run_command(["losetup", "-d", loop_dev])
            except Exception:
                pass

    print("Disk image written:", output_path)
    print("Stage2 LBA:", stage2_start)
    print("Stage2 sectors:", stage2_sectors)


if __name__ == "__main__":
    try:
        main()
    except DiskBuildError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
    except subprocess.CalledProcessError as exc:
        print(f"error: command failed: {exc}", file=sys.stderr)
        sys.exit(exc.returncode)
