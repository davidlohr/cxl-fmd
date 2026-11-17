# SPDX-License-Identifier: LGPL-2.1
"""
man/cxl-fmd.8 says what the program does: every option of --help is in it,
with the same defaults, and it is for the version that is built.
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

MANPAGE = Path(__file__).resolve().parent.parent / "man" / "cxl-fmd.8"


def run(executable, *args):
    return subprocess.run([str(executable), *args], capture_output=True,
                          text=True, timeout=30)


# an option of --help, with its short form or with none (an empty one)
OPTION = r"^\s+(?:-(\w), )?--([\w-]+)"


def test_every_option_is_documented(cxl_fmd_executable):
    usage = run(cxl_fmd_executable, "--help").stdout
    options = re.findall(OPTION, usage, re.M)
    assert len(options) >= 11, usage
    assert ("", "allow-mctp-restricted") in options, usage
    page = MANPAGE.read_text()
    for short, long in options:
        if short:
            assert "\\-" + short in page, f"-{short} is not in the man page"
        assert "\\-\\-" + long.replace("-", "\\-") in page, \
            f"--{long} is not in the man page"


def test_no_option_is_documented_that_is_not_there(cxl_fmd_executable):
    usage = run(cxl_fmd_executable, "--help").stdout
    known = {long for _, long in re.findall(OPTION, usage, re.M)}
    page = MANPAGE.read_text()
    for long in re.findall(r"\\-\\-([a-z]+(?:\\-[a-z]+)*)", page):
        assert long.replace("\\-", "-") in known, f"--{long} is not an option"


def test_defaults_are_the_programs(cxl_fmd_executable):
    """The numbers in --help are the numbers in the page."""
    usage = run(cxl_fmd_executable, "--help").stdout
    page = re.sub(r"\s+", " ", MANPAGE.read_text())
    port = re.search(r"--port PORT\s+HTTP port \(default: (\d+)\)", usage).group(1)
    poll = re.search(r"--event-poll.*?default: (\d+)", usage, re.S).group(1)
    rescan = re.search(r"--rescan.*?default: (\d+)", usage, re.S).group(1)
    threads = re.search(r"--threads.*?1 to (\d+).*?default: (\d+)", usage, re.S)
    assert f"from 1 to 65535. The default is {port}." in page
    assert f"The default is {poll}; 0 disables it." in page
    assert f"The default is {rescan}; 0 disables it." in page
    assert f"from 1 to {threads.group(1)}. The default is {threads.group(2)}." in page


def test_version(cxl_fmd_executable):
    version = run(cxl_fmd_executable, "--version").stdout.split()[-1]
    assert f'"cxl-fmd {version}"' in MANPAGE.read_text().splitlines()[1]


@pytest.mark.skipif(not shutil.which("groff"), reason="groff is not installed")
def test_formats_without_warnings():
    proc = subprocess.run(["groff", "-man", "-ww", "-z", str(MANPAGE)],
                          capture_output=True, text=True, timeout=30)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
