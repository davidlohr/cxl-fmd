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


def section(text, heading):
    """The body of the Markdown section @heading, up to the next heading of
    its level or above."""
    level = len(heading) - len(heading.lstrip("#"))
    start = text.index(heading + "\n")
    end = re.compile(r"^#{1,%d} " % level, re.M).search(text, start + len(heading))
    return text[start:end.start() if end else len(text)]


def flat(text):
    return re.sub(r"\s+", " ", text)


def test_dcd_delete_contract_is_stated():
    """A DELETE of one extent of a sharable tag releases every extent of that
    host with the tag (review DCD-Q A3, kept by the maintainer), and the docs
    say so where DELETE is described, not only by implication."""
    dcd = flat(section((DOCS / "DCD-FM-API.md").read_text(),
                       "#### DELETE of a MemoryExtent"))
    assert ("a `DELETE` of one extent with a tag in a Sharable region "
            "releases every extent that host has with that tag") in dcd, dcd
    assert "`DynamicCapacityRelease`" in dcd
    # the one-line descriptions of the DELETE (endpoint list and table)
    lines = re.findall(r"^.*`DELETE [^`]*MemoryExtents/\{ext\}` *[|-].*$",
                       (DOCS / "Redfish-API.md").read_text(), re.M)
    assert len(lines) >= 2, lines
    for line in lines:
        assert "with the tag" in line, line


def test_zero_tag_is_cxl_fmds_convention():
    """CXL r4.0 gives an all-zero Tag no reserved meaning (review DCD-Q T6):
    the docs call "all zeros is no tag" cxl-fmd's convention, and do not
    give the specification's "Optional" as its reason."""
    text = (DOCS / "DCD-FM-API.md").read_text()
    q5 = flat(text[text.index("5. **Which Tag is \"no tag\"?**"):
                   text.index("6. **One host with accepted")])
    assert "CXL r4.0 gives a zero Tag no reserved meaning" in q5, q5
    assert "**cxl-fmd's convention**" in q5, q5
    for table in ("Table 8-230", "Table 7-72", "Table 7-73", "Table 7-74",
                  "Table 7-75", "Table 7-78"):
        assert table in q5, table
    assert not re.search(r"as the specification calls the Tag\s+\"Optional\"",
                         text), "the zero tag is not the specification's"


def test_trust_boundary_gaps_are_listed():
    """TLS, authentication and the listen address were deferred by the
    maintainer (review F16): the Known Gaps list them as such, with the
    DSP0266 requirements they leave unmet, and the man page says so."""
    text = (DOCS / "Redfish-API.md").read_text()
    gaps = flat(section(text, "## Known Gaps"))
    item = re.search(r" 13\. (.*?) 14\. ", gaps)
    assert item, "no item 13 in the Known Gaps"
    item = item.group(1)
    for words in ("No TLS", "no authentication", "no choice of listen address",
                  "deferred by the maintainer", "Section 13.1.1",
                  "Section 13.3.1", "Section 13.3.2.1"):
        assert words in item, words
    assert "deferred" in flat(section(text, "## Authentication"))
    man = flat((DOCS.parent / "man" / "cxl-fmd.8").read_text())
    assert "there is no TLS or authentication; adding them has been deferred" in man
