# SPDX-License-Identifier: LGPL-2.1
"""
What the docs must keep saying, where getting it wrong would mislead a
reader about what cxl-fmd does.

The deployment sketches in docs/Usecases.md run one cxl-fmd for a fabric
(review F13 C6). The OCP CMS white paper has the MFM Node Agent as "A
daemonset running on each node" and the MFM as "the central control point"
(Section 7); a cxl-fmd on every node would be several Fabric Managers
driving one switch, which nothing in cxl-fmd coordinates.

The contracts the maintainer chose over the review's proposals are stated
plainly: a DELETE of a sharable extent releases the host's whole tag
(DCD-Q A3), an all-zero Tag is no tag only by cxl-fmd's convention (DCD-Q
T6), and TLS, authentication and the listen address are deferred gaps
(F16). The questions for DMTF are drafted, to be submitted by the
maintainer, and the docs do not say they were raised (F12.8, DCD-T5b).
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


def test_the_lock_file_fences_one_host_only():
    """The lock file keeps a second cxl-fmd off one host (review F13.C6b):
    the Known Gaps say it fences nothing more, and why, with the
    specification's words; the man page says so; and the Deployment shares
    the node's lock directory, as a container's /run is its own."""
    text = (DOCS / "Redfish-API.md").read_text()
    gaps = flat(section(text, "## Known Gaps"))
    item = re.search(r" 15\. (.*?)(?: 16\. |$)", gaps)
    assert item, "no item 15 in the Known Gaps"
    item = item.group(1)
    for words in ("One cxl-fmd per host, not per fabric", "--lock-file",
                  "a cxl-fmd on another host", "another Fabric Manager",
                  "is beyond the scope of this specification\" (Section 9.20.1)",
                  "Management of a PBR device requires that a primary FM is "
                  "registered.", "(Section 7.7.12.2)", "Section 8.2.10.8.2",
                  "cxl-fmd sends no PBR command",
                  "is cxl-fmd's reading of these sections"):
        assert words in item, words
    assert "item 15" in flat(section(text, "## Changes for Clients"))
    man = flat((DOCS.parent / "man" / "cxl-fmd.8").read_text())
    assert ".SH ONE FABRIC MANAGER" in man
    assert "another Fabric Manager are not detected" in man
    usecases = (DOCS / "Usecases.md").read_text()
    deployments = [doc for doc in manifests(usecases) if runs_cxl_fmd(doc)
                   and re.search(r"^kind:\s*Deployment", doc, re.M)
                   and re.search(r"^\s+hostNetwork: true", doc, re.M)]
    assert deployments
    for doc in deployments:
        assert re.search(r"mountPath: /run/cxl-fmd\b", doc), doc
        assert re.search(r"hostPath:\s*\n\s+path: /run/cxl-fmd\b", doc), doc


def test_dmtf_questions_are_drafted_not_raised():
    """cxl-fmd cannot raise a standards question itself (review F12.8,
    DCD-T5b): the open questions that need DMTF say a request is drafted,
    to be submitted by the maintainer, and none says it was raised."""
    text = (DOCS / "DCD-FM-API.md").read_text()
    oq = flat(section(text, "## Open Questions"))
    drafted = "is drafted, to be submitted by the maintainer"
    for start, end in (("1. **Units of the `Length`", " 2. **"),
                       ("20. **No DMTF resource for a tag.**", " 21. **"),
                       ("25. **DC sizes below 1 MiB.**", " 26. **"),
                       ("26. **No DSP0288 mapping for 55h and 56h.**", None)):
        item = oq[oq.index(start):]
        item = item[:item.index(end)] if end else item
        assert drafted in item, item
    assert "Open Question 25" in flat(section(text, "### Comments for Table 3"))
    assert "Open Question 26" in flat(text[:text.index("## Table of Contents")])
    for words in ("to be raised with the Redfish Forum",
                  "Whether to propose the properties to DMTF is open"):
        assert words not in flat(text), words
    assert not re.search(r"\b(was|were|has been|have been) (raised|submitted|sent)"
                         r" (to|with) (DMTF|the Redfish Forum)", flat(text))
