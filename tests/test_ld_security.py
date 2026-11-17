# SPDX-License-Identifier: LGPL-2.1
"""
The security actions of CXLLogicalDevice (review F17).

DSP0288 1.4.0 Table 1 maps Set Passphrase, Disable Passphrase, Freeze
Security State, Passphrase Secure Erase and Unlock to the SetPassphrase,
DisablePassphrase, FreezeSecurityState, PassphraseSecureErase and Unlock
actions of CXLLogicalDevice. cxl-fmd served the first four on Memory, as
DSP0288 1.2.0 had them, and Unlock nowhere: a POST of any of them to a
CXLLogicalDevice was 501. They are now served on CXLLogicalDevice, with the
parameters of CXLLogicalDevice_v1, when the LD's CEL lists the command and
it may be sent (review F15: --allow-mctp-restricted for a mock), and the
Memory actions are deprecated aliases of them.

cxl_mld_0 of mock/mld-device.json lists every security command in its
cel_commands, which its LDs have; cxl_mld_asymmetric lists none (the
defaults).
"""

import json
from pathlib import Path

import pytest
import requests

from test_health import daemon  # noqa: F401  (the fixture)

ROOT = Path(__file__).resolve().parent.parent

FLAG = "--allow-mctp-restricted"
LD = "/redfish/v1/Chassis/cxl_mld_0/PCIeDevices/cxl_mld_0/CXLLogicalDevices/1"
MEMORY = "/redfish/v1/Chassis/cxl_mld_0/Memory/cxl_mld_0_ld1"
NO_CEL_LD = ("/redfish/v1/Chassis/cxl_mld_asymmetric/PCIeDevices/cxl_mld_asymmetric"
             "/CXLLogicalDevices/0")
ACTIONS = ["SetPassphrase", "DisablePassphrase", "Unlock", "FreezeSecurityState",
           "PassphraseSecureErase"]
ALIASES = ["SetPassphrase", "SetMasterPassphrase", "DisablePassphrase",
           "DisableMasterPassphrase", "FreezeSecurityState", "SecureEraseUnit"]
BODY = {
    "SetPassphrase": {"PassphraseType": "User", "Passphrase": "x", "NewPassphrase": "y"},
    "DisablePassphrase": {"PassphraseType": "Master", "Passphrase": "x"},
    "Unlock": {"PassphraseType": "User", "Passphrase": "x"},
    "FreezeSecurityState": {},
    "PassphraseSecureErase": {"PassphraseType": "Master", "Passphrase": "x"},
}


def security_actions(api_client, uri, prefix="#CXLLogicalDevice."):
    status, data = api_client.get(uri)
    assert status == 200, uri
    return {k[len(prefix):]: v["target"] for k, v in data.get("Actions", {}).items()
            if k.startswith(prefix)}


def post(api_client, uri, body):
    return requests.post(api_client.base_url + uri, json=body, timeout=30)


@pytest.mark.mock_topology("mock/mld-device.json")
@pytest.mark.daemon_args(FLAG)
class TestAllowed:
    def test_served_on_the_logical_device(self, api_client):
        actions = security_actions(api_client, LD)
        assert actions == {a: f"{LD}/Actions/CXLLogicalDevice.{a}" for a in ACTIONS}

    def test_carried_out(self, api_client):
        for action in ACTIONS:
            response = post(api_client, f"{LD}/Actions/CXLLogicalDevice.{action}",
                            BODY[action])
            assert response.status_code == 200, (action, response.text)

    @pytest.mark.parametrize("action, body", [
        ("Unlock", {"PassphraseType": "Master", "Passphrase": "x"}),
        ("SetPassphrase", {"PassphraseType": "User", "Passphrase": "x"}),
        ("DisablePassphrase", {"Passphrase": "x"}),
        ("DisablePassphrase", {"PassphraseType": "Admin", "Passphrase": "x"}),
        ("PassphraseSecureErase", {"PassphraseType": "User", "Passphrase": "x" * 33}),
    ])
    def test_bad_request(self, api_client, action, body):
        response = post(api_client, f"{LD}/Actions/CXLLogicalDevice.{action}", body)
        assert response.status_code == 400, response.text

    def test_not_there(self, api_client):
        assert post(api_client, f"{LD}/Actions/CXLLogicalDevice.UnlockUnit",
                    BODY["Unlock"]).status_code == 404
        assert post(api_client, LD.replace("/1", "/9") + "/Actions/CXLLogicalDevice.Unlock",
                    BODY["Unlock"]).status_code == 404
        assert post(api_client, f"{MEMORY}/Actions/Memory.UnlockUnit",
                    {"RegionId": "0", "Passphrase": "x"}).status_code == 404

    def test_memory_aliases(self, api_client):
        actions = security_actions(api_client, MEMORY, "#Memory.")
        assert set(actions) == {"ScanMedia", *ALIASES}
        response = post(api_client, f"{MEMORY}/Actions/Memory.SetMasterPassphrase",
                        {"Passphrase": "x", "NewPassphrase": "y"})
        assert response.status_code == 200, response.text
        response = post(api_client, f"{MEMORY}/Actions/Memory.DisablePassphrase",
                        {"Passphrase": "x" * 33})
        assert response.status_code == 400, response.text

    def test_not_in_the_cel(self, api_client):
        assert security_actions(api_client, NO_CEL_LD) == {}
        response = post(api_client, f"{NO_CEL_LD}/Actions/CXLLogicalDevice.Unlock",
                        BODY["Unlock"])
        assert response.status_code == 501, response.text
        assert "Command Effects Log" in response.json()["error"]["message"]


@pytest.mark.mock_topology("mock/mld-device.json")
class TestRefusedOnMctp:
    def test_not_served(self, api_client):
        assert security_actions(api_client, LD) == {}
        assert security_actions(api_client, MEMORY, "#Memory.") == {}

    def test_refused(self, api_client):
        for action in ACTIONS:
            response = post(api_client, f"{LD}/Actions/CXLLogicalDevice.{action}",
                            BODY[action])
            assert response.status_code == 409, (action, response.text)


SECURITY_CEL = ["identify", "get_supported_logs", "get_log", "memdev_identify",
                "get_partition_info", "get_health_info", "set_passphrase",
                "disable_passphrase", "unlock", "freeze_security_state",
                "passphrase_secure_erase"]


@pytest.mark.parametrize("dev", ["cxl_dcd0", "cxl_dcd_mld0"])
def test_dcd_actions_beside(daemon, dev):
    """A DCD's LD has its DC actions and the security actions, side by
    side in one Actions."""
    topology = json.loads((ROOT / "mock" / "dcd-device.json").read_text())
    for d in topology["devices"]:
        if d["id"] == dev:
            d["cel_commands"] = SECURITY_CEL
    base = daemon(topology, FLAG)
    response = requests.get(f"{base}/redfish/v1/Chassis/{dev}/PCIeDevices/{dev}"
                            "/CXLLogicalDevices/0", timeout=30)
    assert response.status_code == 200
    assert set(response.json()["Actions"]) == {
        "#CXLLogicalDevice.DynamicCapacityAdd", "#CXLLogicalDevice.DynamicCapacityRelease",
        *(f"#CXLLogicalDevice.{a}" for a in ACTIONS)}


@pytest.mark.mock_topology("mock/mhd-device.json")
@pytest.mark.daemon_args(FLAG)
def test_pool_ld_of_a_multi_headed_device(api_client):
    """cxl-fmd sends no Memory Device command to a pool LD: none served,
    and a POST is 501 (it was 404)."""
    ld = "/redfish/v1/Chassis/cxl_mhsld_0/PCIeDevices/cxl_mhsld_0/CXLLogicalDevices/0"
    assert security_actions(api_client, ld) == {}
    response = post(api_client, f"{ld}/Actions/CXLLogicalDevice.Unlock", BODY["Unlock"])
    assert response.status_code == 501, response.text
