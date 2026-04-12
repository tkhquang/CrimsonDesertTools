#!/usr/bin/env python3
"""
CrimsonDesertLiveTransmog Version Updater

This script provides a complete solution for version management:
1. Bumps the version in version.hpp (the single source of truth)
2. Updates the CHANGELOG.md file with new release information
3. Can be run manually or through GitHub Actions

Usage:
  python version_updater.py bump [major|minor|patch] [--changelog "Changelog entry"]
  python version_updater.py update-changelog --version X.Y.Z --title "Title" --changelog "Changelog entry"
"""

import re
import os
import sys
import subprocess
from pathlib import Path
import argparse
from datetime import datetime

# Base paths
BASE_DIR = Path(__file__).parent.parent
VERSION_HEADER = BASE_DIR / "src" / "version.hpp"
CHANGELOG_MD = BASE_DIR / "CHANGELOG.md"

def get_current_version():
    """Parse version.hpp to extract version information."""
    if not VERSION_HEADER.exists():
        print(f"Error: {VERSION_HEADER} not found.")
        sys.exit(1)

    version_h = VERSION_HEADER.read_text()

    # Extract version components using defines
    major_match = re.search(r'#define\s+VERSION_MAJOR\s+(\d+)', version_h)
    minor_match = re.search(r'#define\s+VERSION_MINOR\s+(\d+)', version_h)
    patch_match = re.search(r'#define\s+VERSION_PATCH\s+(\d+)', version_h)

    if not (major_match and minor_match and patch_match):
        print("Error: Could not extract version information from version.hpp")
        sys.exit(1)

    major = int(major_match.group(1))
    minor = int(minor_match.group(1))
    patch = int(patch_match.group(1))

    return (major, minor, patch)

def bump_version(part):
    """
    Bump the version in version.hpp.

    Args:
        part: The part of the version to bump: "major", "minor", or "patch"
    """
    major, minor, patch = get_current_version()

    # Bump the specified part
    if part == "major":
        major += 1
        minor = 0
        patch = 0
    elif part == "minor":
        minor += 1
        patch = 0
    elif part == "patch":
        patch += 1
    else:
        print(f"Error: Invalid version part '{part}'. Use 'major', 'minor', or 'patch'.")
        sys.exit(1)

    # Update version.hpp
    version_h = VERSION_HEADER.read_text()

    # Update the defines
    version_h = re.sub(
        r'#define\s+VERSION_MAJOR\s+\d+',
        f'#define VERSION_MAJOR {major}',
        version_h
    )
    version_h = re.sub(
        r'#define\s+VERSION_MINOR\s+\d+',
        f'#define VERSION_MINOR {minor}',
        version_h
    )
    version_h = re.sub(
        r'#define\s+VERSION_PATCH\s+\d+',
        f'#define VERSION_PATCH {patch}',
        version_h
    )

    VERSION_HEADER.write_text(version_h)

    # Return the new version string
    version_str = f"{major}.{minor}.{patch}"
    print(f"Version bumped to {version_str}")
    return version_str

def update_changelog(version, title="", changelog_entry=""):
    """Update CHANGELOG.md with a new version entry and maintain proper structure."""
    changelog_template = """# Changelog

All notable changes to the CrimsonDesertLiveTransmog mod will be documented in this file.

"""

    if not CHANGELOG_MD.exists():
        content = changelog_template
    else:
        content = CHANGELOG_MD.read_text()

        if "# Changelog" in content:
            if "All notable changes" in content:
                parts = content.split("All notable changes to the CrimsonDesertLiveTransmog mod will be documented in this file.")
                if len(parts) > 1:
                    main_content = parts[1].strip()
                    content = changelog_template + main_content
                else:
                    content = changelog_template
            else:
                content = changelog_template + content.split("# Changelog")[1].strip()

    version_header = f"## [{version}]"
    if title:
        version_header += f" - {title}"

    if f"## [{version}]" in content:
        print(f"Warning: Version {version} already exists in changelog, skipping update.")
        return

    header_end_pos = content.find("All notable changes to the CrimsonDesertLiveTransmog mod will be documented in this file.")
    if header_end_pos != -1:
        insert_position = content.find("\n\n", header_end_pos) + 2
    else:
        insert_position = content.find("\n\n", content.find("# Changelog")) + 2

    if changelog_entry:
        formatted_lines = []
        for line in changelog_entry.strip().split('\n'):
            line = line.rstrip()
            formatted_lines.append(line)

        formatted_entry = '\n'.join(formatted_lines)
        new_version_section = f"{version_header}\n\n{formatted_entry}\n\n"
        updated_content = content[:insert_position] + new_version_section + content[insert_position:]

        version_links = []
        link_pattern = r'\[([0-9]+\.[0-9]+\.[0-9]+)\]: (.*)'
        for match in re.finditer(link_pattern, updated_content):
            version_links.append((match.group(1), match.group(2)))

        updated_content = re.sub(r'\n\[[0-9]+\.[0-9]+\.[0-9]+\]: .*', '', updated_content)

        version_in_links = False
        for v, _ in version_links:
            if v == version:
                version_in_links = True
                break

        if not version_in_links:
            version_links.append((version, f"https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v{version}"))

        version_links.sort(key=lambda x: [int(n) for n in x[0].split('.')], reverse=True)

        updated_content = updated_content.rstrip() + "\n"
        for v, link in version_links:
            updated_content += f"\n[{v}]: {link}"
        updated_content += "\n"

        CHANGELOG_MD.write_text(updated_content)
        print(f"Updated {CHANGELOG_MD} with version {version}")
    else:
        print(f"Warning: No changelog entry provided for version {version}, skipping update.")

def main():
    parser = argparse.ArgumentParser(description="CrimsonDesertLiveTransmog Version Manager")
    subparsers = parser.add_subparsers(dest="command", help="Command to execute")

    bump_parser = subparsers.add_parser("bump", help="Bump version and update changelog")
    bump_parser.add_argument("part", choices=["major", "minor", "patch"],
                            help="Version part to bump")
    bump_parser.add_argument("--title", help="Title for the version (e.g., 'Feature Update')")
    bump_parser.add_argument("--changelog", help="Changelog entry for the version")

    changelog_parser = subparsers.add_parser("update-changelog", help="Update changelog with current version")
    changelog_parser.add_argument("--version", help="Version to use (defaults to current version)")
    changelog_parser.add_argument("--title", help="Title for the version (e.g., 'Feature Update')")
    changelog_parser.add_argument("--changelog", required=True, help="Changelog entry for the version")

    args = parser.parse_args()

    if args.command == "bump":
        version = bump_version(args.part)
        if args.changelog:
            update_changelog(version, args.title, args.changelog)
        print(f"\nSuccessfully bumped version to {version}")
    elif args.command == "update-changelog":
        if args.version:
            version = args.version
        else:
            major, minor, patch = get_current_version()
            version = f"{major}.{minor}.{patch}"
        update_changelog(version, args.title, args.changelog)
        print(f"\nSuccessfully updated changelog for version {version}")
    else:
        parser.print_help()

if __name__ == "__main__":
    main()
