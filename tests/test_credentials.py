import pytest

from protocol.credentials import acl_block, generate_password, server_acl_block
from protocol.errors import ProtocolError


def test_password_is_long_enough():
    assert len(generate_password()) >= 24


def test_passwords_are_unique():
    assert len({generate_password() for _ in range(100)}) == 100


def test_password_is_url_safe():
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
    assert set(generate_password()) <= allowed


def test_acl_block_confines_node_to_own_topics():
    block = acl_block("probe-a4c1f8")
    assert "user probe-a4c1f8" in block
    assert "topic write nms/v1/node/probe-a4c1f8/result" in block
    assert "topic write nms/v1/node/probe-a4c1f8/monitor" in block
    assert "topic write nms/v1/node/probe-a4c1f8/status" in block
    assert "topic write nms/v1/node/probe-a4c1f8/telemetry" in block
    assert "topic write nms/v1/announce" in block
    assert "topic read nms/v1/node/probe-a4c1f8/cmd" in block


def test_acl_block_grants_no_access_to_other_nodes():
    block = acl_block("probe-a4c1f8")
    assert "probe-7e2b10" not in block
    assert "nms/v1/#" not in block
    assert "node/+/" not in block


def test_acl_block_validates_node_id():
    with pytest.raises(ProtocolError):
        acl_block("probe-NOPE")


def test_server_acl_block_reads_all_and_writes_commands():
    block = server_acl_block("nms-server")
    assert "user nms-server" in block
    assert "topic read nms/v1/#" in block
    assert "topic write nms/v1/node/+/cmd" in block
