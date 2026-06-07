#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""
Dependency installer for Valecium OS development.

Detects the Linux distribution and installs required packages.
"""

import argparse
import multiprocessing
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

Dependencies = {
    "debian": {
        "packages": [
            # === Core Build Tools ===
            "build-essential",
            "gcc",
            "g++",
            "make",
            # === Development Libraries (Required for Toolchain Build) ===
            "libmpfr-dev",
            "libgmp-dev",
            "libmpc-dev",
            # === Python & Scripting ===
            "python3",
            "python3-pil",
            "scons",
            # === Boot & Image Creation ===
            "xorriso",
            "grub-pc-bin",
            # === Emulation & Debugging ===
            "qemu-system-x86",
            "qemu-system-aarch64",
            # === Code Quality ===
            "clang-format",
            # === Documentation ===
            "pandoc",
            "asciidoctor",
            "texinfo",
        ],
        "update_cmd": ["apt-get", "update"],
        "install_cmd": ["apt-get", "install", "-y"],
    },
    "fedora": {
        "packages": [
            # === Core Build Tools ===
            "gcc",
            "gcc-c++",
            "make",
            # === Development Libraries (Required for Toolchain Build) ===
            "mpfr-devel",
            "gmp-devel",
            "libmpc-devel",
            # === Python & Scripting ===
            "python3",
            "python3-pip",
            "scons",
            "bash",
            "grep",
            "coreutils",
            # === Boot & Image Creation ===
            "xorriso",
            "grub2-tools",
            "grub2-tools-extra",
            # === Emulation & Debugging ===
            "qemu-system-x86",
            "qemu-system-aarch64",
            "gdb",
            # === Code Quality ===
            "clang-tools-extra",
            # === Documentation ===
            "pandoc",
            "asciidoctor",
            "texinfo",
        ],
        "update_cmd": None,
        "install_cmd": ["dnf", "install", "-y"],
    },
    "arch": {
        "packages": [
            # === Core Build Tools ===
            "base-devel",
            "gcc",
            "make",
            # === Development Libraries (Required for Toolchain Build) ===
            "mpfr",
            "gmp",
            "libmpc",
            # === Python & Scripting ===
            "python",
            "python-pip",
            "scons",
            "bash",
            "grep",
            "coreutils",
            # === Boot & Image Creation ===
            "xorriso",
            "grub",
            # === Emulation & Debugging ===
            "qemu-system-x86",
            "qemu-system-aarch64",
            "gdb",
            # === Code Quality ===
            "clang",
            # === Documentation ===
            "pandoc",
            "asciidoctor",
            "texinfo",
        ],
        "update_cmd": ["pacman", "-Syu", "--noconfirm"],
        "install_cmd": ["pacman", "-S", "--noconfirm"],
    },
    "suse": {
        "packages": [
            # === Core Build Tools ===
            "gcc",
            "gcc-c++",
            "make",
            # === Development Libraries (Required for Toolchain Build) ===
            "mpfr-devel",
            "gmp-devel",
            "libmpc-devel",
            # === Python & Scripting ===
            "python3",
            "python3-pip",
            "scons",
            "bash",
            "grep",
            "coreutils",
            # === Boot & Image Creation ===
            "xorriso",
            "grub2",
            "grub2-i386-pc",
            # === Emulation & Debugging ===
            "qemu-x86",
            "qemu-system-aarch64",
            "gdb",
            # === Code Quality ===
            "clang",
            # === Documentation ===
            "pandoc",
            "asciidoctor",
            "texinfo",
        ],
        "update_cmd": None,
        "install_cmd": ["zypper", "install", "-y"],
    },
    "alpine": {
        "packages": [
            # === Core Build Tools ===
            "build-base",
            "gcc",
            "make",
            # === Development Libraries (Required for Toolchain Build) ===
            "mpfr-dev",
            "mpc1-dev",
            "gmp-dev",
            # === Python & Scripting ===
            "python3",
            "py3-pip",
            "scons",
            "bash",
            "grep",
            "coreutils",
            # === Boot & Image Creation ===
            "xorriso",
            "grub",
            # === Emulation & Debugging ===
            "qemu-system-x86_64",
            "qemu-system-aarch64",
            "gdb",
            # === Code Quality ===
            "clang",
            # === Documentation ===
            "pandoc",
            "asciidoctor",
            "texinfo",
        ],
        "update_cmd": ["apk", "update"],
        "install_cmd": ["apk", "add"],
    },
}


def GetCpuCount() -> int:
    """Get number of CPUs for parallel runtime builds."""
    return multiprocessing.cpu_count()


def RunCommand(
    Cmd: list, Cwd: str = None, Env: dict = None
) -> subprocess.CompletedProcess:
    """Run a command with logging."""
    print(f"$ {' '.join(Cmd)}")
    MergedEnv = os.environ.copy()
    if Env:
        MergedEnv.update(Env)
    return subprocess.run(Cmd, cwd=Cwd, env=MergedEnv, check=True)


def DownloadFile(Url: str, Dest: Path):
    """Download a file if it does not already exist."""
    if Dest.exists():
        print(f"Already downloaded: {Dest.name}")
        return

    print(f"Downloading: {Url}")
    urllib.request.urlretrieve(Url, str(Dest))


def ExtractArchive(Archive: Path, DestDir: Path):
    """Extract a source archive if needed."""
    print(f"Extracting: {Archive.name}")
    with tarfile.open(Archive) as Tar:
        Tar.extractall(str(DestDir))


def DetectDistro() -> str:
    """Detect the Linux distribution family."""
    # Check for package managers
    if shutil.which("apt-get"):
        return "debian"
    elif shutil.which("dnf"):
        return "fedora"
    elif shutil.which("yum"):
        return "fedora"
    elif shutil.which("pacman"):
        return "arch"
    elif shutil.which("zypper"):
        return "suse"
    elif shutil.which("apk"):
        return "alpine"

    # Fallback: check /etc/os-release
    if os.path.exists("/etc/os-release"):
        with open("/etc/os-release") as f:
            Content = f.read().lower()
            if "debian" in Content or "ubuntu" in Content:
                return "debian"
            elif "fedora" in Content or "rhel" in Content or "centos" in Content:
                return "fedora"
            elif "arch" in Content:
                return "arch"
            elif "suse" in Content:
                return "suse"
            elif "alpine" in Content:
                return "alpine"

    return None


def InstallDependencies(Distro: str, DryRun: bool = False, UseSudo: bool = True) -> int:
    """Install dependencies for the specified distribution.

    Args:
        Distro: Distribution family name
        DryRun: If True, only print commands without executing
        UseSudo: If True, prefix commands with sudo

    Returns:
        0 on success, non-zero on error
    """
    if Distro not in Dependencies:
        print(f"Error: Unknown distribution: {Distro}", file=sys.stderr)
        print(f"Supported: {', '.join(Dependencies.keys())}", file=sys.stderr)
        return 1

    Config = Dependencies[Distro]
    Packages = Config["packages"]

    def RunCmd(Cmd):
        if UseSudo and os.geteuid() != 0:
            Cmd = ["sudo"] + Cmd

        print(f"$ {' '.join(Cmd)}")
        if not DryRun:
            return subprocess.call(Cmd)
        return 0

    # Update package lists if needed
    if Config["update_cmd"]:
        Result = RunCmd(Config["update_cmd"])
        if Result != 0 and not DryRun:
            return Result

    # Install packages
    InstallCmd = Config["install_cmd"] + Packages
    Result = RunCmd(InstallCmd)

    return Result


def main():
    parser = argparse.ArgumentParser(
        description="Install development dependencies for Valecium OS",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    parser.add_argument(
        "-d",
        "--distro",
        choices=list(Dependencies.keys()),
        help="Force specific distribution (auto-detected by default)",
    )
    parser.add_argument(
        "-t", "--target", help="Target triple for runtime dependency build"
    )
    parser.add_argument(
        "--output", help="Output directory for runtime dependency artifacts"
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=GetCpuCount(),
        help=f"Parallel jobs for runtime build (default: {GetCpuCount()})",
    )
    parser.add_argument(
        "-n", "--dry-run", action="store_true", help="Print commands without executing"
    )
    parser.add_argument(
        "--no-sudo", action="store_true", help="Run commands without sudo"
    )
    parser.add_argument(
        "-l",
        "--list",
        action="store_true",
        help="List packages for detected/specified distribution",
    )
    parser.add_argument(
        "-y",
        "--yes",
        action="store_true",
        help="Skip confirmation prompt (for non-interactive use)",
    )

    Args = parser.parse_args()

    # Detect or use specified distro
    Distro = Args.distro or DetectDistro()
    if not Distro:
        print("Error: Could not detect Linux distribution.", file=sys.stderr)
        print("Please specify with --distro", file=sys.stderr)
        sys.exit(1)

    print(f"Distribution: {Distro}")

    if Args.list:
        print(f"\nPackages for {Distro}:")
        for Pkg in Dependencies[Distro]["packages"]:
            print(f"  - {Pkg}")
        sys.exit(0)

    print()
    if not Args.dry_run and not Args.yes:
        UserResponse = input("Install dependencies? [y/N] ").strip().lower()
        if UserResponse not in ("y", "yes"):
            print("Cancelled.")
            sys.exit(0)

    sys.exit(
        InstallDependencies(
            Distro=Distro,
            DryRun=Args.dry_run,
            UseSudo=not Args.no_sudo,
        )
    )


if __name__ == "__main__":
    main()
