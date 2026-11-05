############################################################################
# tools/espressif/check_esptool.py
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################
import argparse
import re
import shutil
import subprocess
import sys


def parse_version(version_string) -> tuple:
    """Extract a major/minor/patch version, accepting omitted patch zero."""
    match = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", version_string)

    if match:
        return tuple(int(part or 0) for part in match.groups())
    return ()


def check_version(min_esptool_version: str, esptool: str = None) -> bool:
    """Check the selected esptool executable without importing its package.

    Return True if the executable is missing, fails, or is too old.
    CMake passes the exact executable used for image generation/flashing;
    Make callers retain PATH discovery, including the legacy .py name.
    """
    esptool = esptool or shutil.which("esptool") or shutil.which("esptool.py")
    if not esptool:
        print("esptool not found. Please run: 'pip install esptool'")
        return True

    try:
        result = subprocess.run(
            [esptool, "version"], capture_output=True, text=True, check=True
        )
        version_str = result.stdout.strip()
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"Cannot check {esptool}: {error}")
        return True

    esptool_version = parse_version(version_str)
    minimum = parse_version(min_esptool_version)

    if esptool_version and minimum and esptool_version >= minimum:
        return False

    print("Unsupported esptool version:", version_str,
          "expects >=", min_esptool_version)
    print("Upgrade using: 'pip install --upgrade esptool' and run 'make' again")
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Check the esptool version")
    parser.add_argument("-v", "--version", required=True)
    parser.add_argument("--esptool", help="exact esptool executable to check")
    args = parser.parse_args()
    sys.exit(check_version(args.version, args.esptool))
