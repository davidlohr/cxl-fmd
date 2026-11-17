# SPDX-License-Identifier: LGPL-2.1
"""
The deployment sketches in docs/Usecases.md run one cxl-fmd for a fabric
(review F13 C6). The OCP CMS white paper has the MFM Node Agent as "A
daemonset running on each node" and the MFM as "the central control point"
(Section 7); a cxl-fmd on every node would be several Fabric Managers
driving one switch, which nothing in cxl-fmd coordinates.
"""

import re
from pathlib import Path

DOCS = Path(__file__).resolve().parent.parent / "docs"


def manifests(text):
    """Each Kubernetes object of each ```yaml block of @text."""
    for block in re.findall(r"^```yaml\n(.*?)^```", text, re.M | re.S):
        for doc in re.split(r"^---\s*$", block, flags=re.M):
            if re.search(r"^kind:", doc, re.M):
                yield doc


def runs_cxl_fmd(doc):
    return bool(re.search(r"^\s*(- )?name: cxl-fmd\s*$", doc, re.M) or
                re.search(r"^\s*image: \S*cxl-fmd:", doc, re.M))


def test_no_cxl_fmd_per_node():
    """No DaemonSet runs cxl-fmd, and a Deployment of it has one replica,
    replaced, not rolled."""
    text = (DOCS / "Usecases.md").read_text()
    found = 0
    for doc in manifests(text):
        kind = re.search(r"^kind:\s*(\S+)", doc, re.M).group(1)
        if not runs_cxl_fmd(doc):
            continue
        assert kind != "DaemonSet", doc
        if kind == "Deployment":
            found += 1
            assert re.search(r"^\s+replicas: 1\b", doc, re.M), doc
            assert re.search(r"^\s+type: Recreate\b", doc, re.M), doc
    assert found >= 1


def test_high_availability_is_not_promised():
    """cxl-fmd has no fail-over, lease or fencing: where the docs name high
    availability, they say it is not implemented."""
    for path in DOCS.glob("*.md"):
        for line_no, line in enumerate(path.read_text().splitlines(), 1):
            if re.search(r"high[- ]availability", line, re.I):
                assert "not implemented" in line.lower(), f"{path.name}:{line_no}: {line}"
