# SPDX-License-Identifier: LGPL-2.1
"""
The error responses of cxl-fmd, checked against the DMTF Base registry
(review F18 claim 2).

"The @Message.ExtendedInfo property should be present in all error
responses" (DSP0266 1.25.0 Section 9.5.10). Each message in it is checked
against schema/dmtf/registries/Base.1.24.0.json, a copy of the registry as
the DMTF publishes it: its MessageId is
"<MessageRegistryPrefix>.<MajorVersion>.<MinorVersion>.<MessageKey>"
(Section 9.5.11.2), its key is in the registry, its MessageArgs are as many
as the message's NumberOfArgs ("If the parameterized message defines a
MessageId, the service shall include the MessageArgs in the response",
Table 20), its Message is the registry's with them put in, and its
MessageSeverity is the registry's. GeneralError has a Resolution of
cxl-fmd's: "When an implementation uses GeneralError from the Base Message
Registry in ExtendedInfo, the implementation should include a
service-defined value for the Resolution property" (Section 9.5.11.2).
"""

import json
import re
from pathlib import Path

REGISTRY_FILE = (Path(__file__).parent.parent / "schema" / "dmtf" / "registries"
                 / "Base.1.24.0.json")
REGISTRY = json.loads(REGISTRY_FILE.read_text())
PREFIX = "Base.1.24."


def registry_message(key, args):
    """The registry's Message of @key with @args put in for %1, %2, ..."""
    text = REGISTRY["Messages"][key]["Message"]
    return re.sub(r"%(\d)", lambda m: args[int(m.group(1)) - 1], text)


def check_message(m):
    """@m is a message of the Base registry as cxl-fmd gives one; its key"""
    assert m["MessageId"].startswith(PREFIX), m
    key = m["MessageId"][len(PREFIX):]
    assert key in REGISTRY["Messages"], f"{key} is not in {REGISTRY_FILE.name}"
    entry = REGISTRY["Messages"][key]
    args = m.get("MessageArgs", [])
    assert len(args) == entry["NumberOfArgs"], m
    assert all(isinstance(a, str) for a in args), m
    assert m["Message"] == registry_message(key, args), m
    assert m["MessageSeverity"] == entry["MessageSeverity"], m
    assert m["Severity"] == m["MessageSeverity"], m
    assert m["Resolution"], m
    if key == "GeneralError":
        assert m["Resolution"] != entry["Resolution"], \
            f"GeneralError with no Resolution of cxl-fmd's: {m}"
    for p in m.get("RelatedProperties", []):
        assert p.startswith("/"), m
    return key


def check_error(body):
    """
    @body is an error response of cxl-fmd: a code of the registry, a
    message, and ExtendedInfo of Base messages. Returns their keys.
    """
    error = body["error"]
    assert error["code"] == PREFIX + "GeneralError", error
    assert isinstance(error["message"], str) and error["message"], error
    info = error["@Message.ExtendedInfo"]
    assert isinstance(info, list) and info, error
    return [check_message(m) for m in info]
