#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""
Code formatter for Valecium OS.

Formats C/header files using clang-format and Python/SCons files using ruff.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path


# File extensions to format
SOURCE_EXTENSIONS = {".c", ".h"}
PYTHON_EXTENSIONS = {".py"}
SCONS_FILENAMES = {"SConstruct", "SConscript"}

RUFF_CONFIG_PATH = Path(__file__).resolve().parent.parent / "format" / "ruff.toml"
CLANG_CONFIG_PATH = (
    Path(__file__).resolve().parent.parent / "format" / "clang-format.yaml"
)

# Directories to skip
SKIP_DIRS = {"build", "toolchain", ".git", "__pycache__", "node_modules"}


def classify_file(filepath: str):
    """Classify a file for clang-format or ruff."""
    name = Path(filepath).name
    ext = Path(filepath).suffix.lower()
    if ext in SOURCE_EXTENSIONS:
        return "clang"
    if ext in PYTHON_EXTENSIONS or name in SCONS_FILENAMES:
        return "ruff"
    return None


def find_source_files(root_dir: str) -> dict:
    """Find all supported source files in a directory tree.

    Args:
        root_dir: Root directory to search

    Returns:
        Dict with keys "clang" and "ruff"
    """
    files = {"clang": [], "ruff": []}

    for dirpath, dirnames, filenames in os.walk(root_dir):
        # Remove directories to skip
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]

        for filename in filenames:
            filepath = os.path.join(dirpath, filename)
            kind = classify_file(filepath)
            if kind:
                files[kind].append(filepath)

    return files


def format_c_files(
    files: list,
    formatter: str = "clang-format",
    config_path: Path = None,
    check_only: bool = False,
    verbose: bool = False,
) -> int:
    """Format C/header files using clang-format.

    Args:
        files: List of file paths to format
        formatter: Formatter command name
        config_path: Path to a .clang-format config file
        check_only: If True, only check formatting without modifying
        verbose: Print each file being processed

    Returns:
        0 if successful, 1 if changes needed (check_only) or errors
    """
    if not files:
        print("No C/header files found.")
        return 0

    # Build command
    cmd = [formatter]
    if config_path and config_path.is_file():
        cmd.extend(["--style", f"file:{config_path}"])
    if check_only:
        cmd.extend(["--dry-run", "--Werror"])
    else:
        cmd.append("-i")

    errors = 0

    for filepath in files:
        if verbose:
            action = "Checking" if check_only else "Formatting"
            print(f"{action}: {filepath}")

        result = subprocess.run(cmd + [filepath], capture_output=True)
        if result.returncode != 0:
            errors += 1
            if check_only:
                print(f"Needs formatting: {filepath}")
            else:
                print(f"Error formatting: {filepath}")
                if result.stderr:
                    print(result.stderr.decode())

    if check_only:
        if errors > 0:
            print(f"\n{errors} file(s) need formatting.")
            return 1
        else:
            print("All C/header files properly formatted.")
            return 0
    else:
        if errors > 0:
            print(f"\n{errors} file(s) had errors.")
            return 1
        else:
            print(f"Formatted {len(files)} C/header file(s).")
            return 0


def format_python_files(
    files: list,
    formatter: str = "ruff",
    config_path: Path = None,
    check_only: bool = False,
    verbose: bool = False,
) -> int:
    """Format Python/SCons files using ruff.

    Args:
        files: List of file paths to format
        formatter: Formatter command name
        config_path: Path to ruff.toml config
        check_only: If True, only check formatting without modifying
        verbose: Print each file being processed

    Returns:
        0 if successful, 1 if changes needed (check_only) or errors
    """
    if not files:
        print("No Python/SCons files found.")
        return 0

    cmd = [formatter, "format"]
    if config_path:
        cmd.extend(["--config", str(config_path)])
    if check_only:
        cmd.append("--check")

    errors = 0

    for filepath in files:
        if verbose:
            action = "Checking" if check_only else "Formatting"
            print(f"{action}: {filepath}")

        result = subprocess.run(cmd + [filepath], capture_output=True)
        if result.returncode != 0:
            errors += 1
            if check_only:
                print(f"Needs formatting: {filepath}")
            else:
                print(f"Error formatting: {filepath}")
            if result.stderr:
                print(result.stderr.decode())

    if check_only:
        if errors > 0:
            print(f"\n{errors} file(s) need formatting.")
            return 1
        else:
            print("All Python/SCons files properly formatted.")
            return 0
    else:
        if errors > 0:
            print(f"\n{errors} file(s) had errors.")
            return 1
        else:
            print(f"Formatted {len(files)} Python/SCons file(s).")
            return 0


def main():
    parser = argparse.ArgumentParser(
        description="Format C/header and Python/SCons files in the Valecium OS project",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    parser.add_argument(
        "paths",
        nargs="*",
        default=["."],
        help="Directories or files to format (default: current directory)",
    )
    parser.add_argument(
        "-c",
        "--check",
        action="store_true",
        help="Check formatting without modifying files",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Print each file being processed"
    )
    parser.add_argument(
        "--formatter",
        default="clang-format",
        help="Formatter command (default: clang-format)",
    )

    args = parser.parse_args()

    # Collect all files to format
    clang_files = []
    ruff_files = []
    for path in args.paths:
        if os.path.isfile(path):
            kind = classify_file(path)
            if kind == "clang":
                clang_files.append(path)
            elif kind == "ruff":
                ruff_files.append(path)
            else:
                print(f"Warning: Unsupported file type: {path}", file=sys.stderr)
        elif os.path.isdir(path):
            found = find_source_files(path)
            clang_files.extend(found["clang"])
            ruff_files.extend(found["ruff"])
        else:
            print(f"Warning: Path not found: {path}", file=sys.stderr)

    if not clang_files and not ruff_files:
        print("No files to format.")
        sys.exit(0)

    c_result = format_c_files(
        files=clang_files,
        formatter=args.formatter,
        config_path=CLANG_CONFIG_PATH,
        check_only=args.check,
        verbose=args.verbose,
    )
    ruff_result = format_python_files(
        files=ruff_files,
        config_path=RUFF_CONFIG_PATH if RUFF_CONFIG_PATH.is_file() else None,
        check_only=args.check,
        verbose=args.verbose,
    )

    sys.exit(1 if (c_result != 0 or ruff_result != 0) else 0)


if __name__ == "__main__":
    main()
