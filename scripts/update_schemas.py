#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1
"""
Download DMTF Redfish schemas and set up installed symlinks.

This script downloads the official DMTF schema bundle and extracts
the CSDL (XML) and JSON schema files. Existing installed symlinks
are preserved across updates; if none exist, defaults are installed.

Custom/OEM schemas (files not in the DMTF bundle) are preserved.

Usage:
    python3 scripts/update_schemas.py
"""

import os
import shutil
import zipfile
from collections import OrderedDict, defaultdict
from io import BytesIO

import requests

import generate_enums

# DMTF Schema version to download
VERSION = "DSP8010_2026.2"

# Default schemas to install if no symlinks exist
# These are the schemas used by cxl-fmd for CXL device management
DEFAULT_INSTALLED_SCHEMAS = [
    # Core resources
    "ServiceRoot_v1.xml",
    "Resource_v1.xml",
    "RedfishExtensions_v1.xml",
    "Message_v1.xml",
    "MessageRegistry_v1.xml",
    "Privileges_v1.xml",
    "odata_v4.xml",
    "odata-v4.xml",

    # Session/Manager
    "SessionCollection_v1.xml",
    "Session_v1.xml",
    "ManagerCollection_v1.xml",
    "Manager_v1.xml",
    "ComputerSystemCollection_v1.xml",
    "ComputerSystem_v1.xml",

    # Collections
    "ChassisCollection_v1.xml",
    "PCIeDeviceCollection_v1.xml",
    "MemoryDomainCollection_v1.xml",
    "MemoryChunksCollection_v1.xml",
    "CXLLogicalDeviceCollection_v1.xml",

    # Resources for Phase 1 (Direct-attached SLD/MLD)
    "Chassis_v1.xml",
    "PCIeDevice_v1.xml",
    "PCIeFunction_v1.xml",
    "MemoryDomain_v1.xml",
    "MemoryChunks_v1.xml",
    "CXLLogicalDevice_v1.xml",

    # Phase 2 (Fabric/Switch)
    "FabricCollection_v1.xml",
    "Fabric_v1.xml",
    "SwitchCollection_v1.xml",
    "Switch_v1.xml",
    "PortCollection_v1.xml",
    "Port_v1.xml",
    "EndpointCollection_v1.xml",
    "Endpoint_v1.xml",
    "ConnectionCollection_v1.xml",
    "Connection_v1.xml",
    "MemoryRegionCollection_v1.xml",
    "MemoryRegion_v1.xml",
    "MemoryExtentCollection_v1.xml",
    "MemoryExtent_v1.xml",

    # Virtual CXL Switch (VCS) resources
    "VirtualCXLSwitchCollection_v1.xml",
    "VirtualCXLSwitch_v1.xml",
    "VirtualPCI2PCIBridgeCollection_v1.xml",
    "VirtualPCI2PCIBridge_v1.xml",
]

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
PROJECT_ROOT = os.path.realpath(os.path.join(SCRIPT_DIR, ".."))

SCHEMA_PATH = os.path.join(PROJECT_ROOT, "schema", "dmtf", "csdl")
JSON_SCHEMA_PATH = os.path.join(PROJECT_ROOT, "schema", "dmtf", "json-schema")


def backup_custom_files(directory, dmtf_filenames):
    """
    Backup custom/OEM files that are not in the DMTF bundle.

    Returns a dict of {filename: content} for files to restore.
    """
    custom_files = {}
    if not os.path.exists(directory):
        return custom_files

    for filename in os.listdir(directory):
        filepath = os.path.join(directory, filename)
        if os.path.isfile(filepath) and filename not in dmtf_filenames:
            print(f"  Preserving custom schema: {filename}")
            with open(filepath, "rb") as f:
                custom_files[filename] = f.read()

    return custom_files


def restore_custom_files(directory, custom_files):
    """Restore custom/OEM files after schema update."""
    for filename, content in custom_files.items():
        filepath = os.path.join(directory, filename)
        with open(filepath, "wb") as f:
            f.write(content)
        print(f"  Restored custom schema: {filename}")

# Installed symlinks directories
SCHEMA_INSTALLED_PATH = os.path.join(SCHEMA_PATH, "..", "installed")
JSON_SCHEMA_INSTALLED_PATH = os.path.join(JSON_SCHEMA_PATH + "-installed")


class SchemaVersion:
    """
    A Python class for sorting Redfish schema versions.

    Allows sorting Redfish versions in the way humans expect, by comparing
    version strings as lists (ie 0_2_0 comes before 0_10_0). It does case
    insensitive schema name comparisons.
    """

    def __init__(self, key):
        key = str.casefold(key)
        split_tup = key.split(".")
        self.version_pieces = [split_tup[0]]
        if len(split_tup) < 2:
            return
        version = split_tup[1]
        if version.startswith("v"):
            version = version[1:]
        if any(char.isdigit() for char in version):
            self.version_pieces.extend([int(x) for x in version.split("_")])

    def __lt__(self, other):
        return self.version_pieces < other.version_pieces


def download_schemas():
    """Download DMTF schema bundle."""
    print(f"Downloading {VERSION}...")

    proxies = {"https": os.environ.get("https_proxy", None)}
    url = (
        "https://www.dmtf.org/sites/default/files/standards/documents/"
        + VERSION
        + ".zip"
    )

    r = requests.get(url, proxies=proxies)
    r.raise_for_status()

    return zipfile.ZipFile(BytesIO(r.content))


def main():
    print(f"Updating schemas from DMTF {VERSION}")
    print(f"Project root: {PROJECT_ROOT}")

    zip_ref = download_schemas()

    # Collect filenames from zip
    csdl_filenames = []
    json_schema_files = defaultdict(list)

    for zip_file in zip_ref.infolist():
        if zip_file.is_dir():
            continue
        if zip_file.filename.startswith(VERSION + "/csdl/"):
            csdl_filenames.append(os.path.basename(zip_file.filename))
        elif zip_file.filename.startswith(VERSION + "/json-schema/"):
            filename = os.path.basename(zip_file.filename)
            filenamesplit = filename.split(".")
            json_schema_files[filenamesplit[0]].append(filename)

    # Sort JSON files by version
    for key, value in json_schema_files.items():
        value.sort(key=SchemaVersion, reverse=True)

    # Create ordered dictionary by schema name
    json_schema_files = OrderedDict(
        sorted(json_schema_files.items(), key=lambda x: SchemaVersion(x[0]))
    )

    # Get currently-installed CSDL symlinks (preserve across update)
    # If none exist, use defaults
    csdl_installed_symlinks = set()
    if os.path.exists(SCHEMA_INSTALLED_PATH):
        for csdl_symlink in os.listdir(SCHEMA_INSTALLED_PATH):
            csdl_installed_symlinks.add(csdl_symlink)

    if not csdl_installed_symlinks:
        print("No existing CSDL symlinks found, using defaults")
        csdl_installed_symlinks = set(DEFAULT_INSTALLED_SCHEMAS)

    # Get currently-installed JSON symlinks (preserve across update)
    json_base_symlinks = defaultdict(list)
    if os.path.exists(JSON_SCHEMA_INSTALLED_PATH):
        for json_symlink in os.listdir(JSON_SCHEMA_INSTALLED_PATH):
            base_json_name = json_symlink.split(".")[0]
            json_base_symlinks[base_json_name].append(json_symlink)

    # If no JSON symlinks, derive from CSDL list
    if not json_base_symlinks:
        print("No existing JSON symlinks found, deriving from CSDL list")
        for csdl in csdl_installed_symlinks:
            base_name = csdl.replace("_v1.xml", "")
            json_base_symlinks[base_name] = []  # Will be filled during extraction

    # Backup custom/OEM schemas before clearing directories
    csdl_dmtf_set = set(csdl_filenames)
    json_dmtf_set = set()
    for versions in json_schema_files.values():
        json_dmtf_set.update(versions)

    custom_csdl = backup_custom_files(SCHEMA_PATH, csdl_dmtf_set)
    custom_json = backup_custom_files(JSON_SCHEMA_PATH, json_dmtf_set)

    # Clear and recreate schema directories
    if os.path.exists(SCHEMA_PATH):
        shutil.rmtree(SCHEMA_PATH)
    os.makedirs(SCHEMA_PATH)

    if os.path.exists(JSON_SCHEMA_PATH):
        shutil.rmtree(JSON_SCHEMA_PATH)
    os.makedirs(JSON_SCHEMA_PATH)

    if os.path.exists(SCHEMA_INSTALLED_PATH):
        shutil.rmtree(SCHEMA_INSTALLED_PATH)
    os.makedirs(SCHEMA_INSTALLED_PATH)

    if os.path.exists(JSON_SCHEMA_INSTALLED_PATH):
        shutil.rmtree(JSON_SCHEMA_INSTALLED_PATH)
    os.makedirs(JSON_SCHEMA_INSTALLED_PATH)

    # Extract CSDL files and recreate installed symlinks
    for csdl_file in csdl_filenames:
        with open(os.path.join(SCHEMA_PATH, csdl_file), "wb") as schema_out:
            content = zip_ref.read(os.path.join(VERSION + "/csdl", csdl_file))
            content = content.replace(b"\r\n", b"\n")
            schema_out.write(content)

        if csdl_file in csdl_installed_symlinks:
            os.symlink(
                os.path.join("..", "csdl", csdl_file),
                os.path.join(SCHEMA_INSTALLED_PATH, csdl_file),
            )

    # Restore custom CSDL schemas
    restore_custom_files(SCHEMA_PATH, custom_csdl)

    print(f"Extracted {len(csdl_filenames)} CSDL files")

    # Extract JSON schema files and recreate installed symlinks
    for schema_filename, versions in json_schema_files.items():
        zip_filepath = os.path.join(VERSION + "/json-schema", versions[0])

        with open(
            os.path.join(JSON_SCHEMA_PATH, versions[0]), "wb"
        ) as schema_file:
            content = zip_ref.read(zip_filepath)
            content = content.replace(b"\r\n", b"\n")
            schema_file.write(content)

        if schema_filename in json_base_symlinks:
            os.symlink(
                os.path.join("..", "json-schema", versions[0]),
                os.path.join(JSON_SCHEMA_INSTALLED_PATH, versions[0]),
            )

    # Restore custom JSON schemas
    restore_custom_files(JSON_SCHEMA_PATH, custom_json)

    installed_csdl_count = len(os.listdir(SCHEMA_INSTALLED_PATH))
    installed_json_count = len(os.listdir(JSON_SCHEMA_INSTALLED_PATH))

    print(f"Extracted {len(json_schema_files)} JSON schema files")
    print(f"Installed {installed_csdl_count} CSDL symlinks")
    print(f"Installed {installed_json_count} JSON symlinks")

    zip_ref.close()

    # Generate C enums from schemas
    print("\nGenerating C enums...")
    generate_enums.main()

    print("\nDone!")


if __name__ == "__main__":
    main()
