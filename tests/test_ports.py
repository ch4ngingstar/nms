import pytest

from protocol.errors import ProtocolError
from protocol.ports import parse_ports


def test_single_port():
    assert parse_ports("22") == [22]


def test_comma_separated_list():
    assert parse_ports("22,80,443") == [22, 80, 443]


def test_inclusive_range():
    assert parse_ports("20-23") == [20, 21, 22, 23]


def test_mixed_list_and_ranges():
    assert parse_ports("22,80,443,8000-8002") == [22, 80, 443, 8000, 8001, 8002]


def test_result_is_sorted_and_deduplicated():
    assert parse_ports("80,22,80,20-22") == [20, 21, 22, 80]


def test_surrounding_whitespace_tolerated():
    assert parse_ports(" 22 , 80 - 82 ") == [22, 80, 81, 82]


def test_boundary_ports_allowed():
    assert parse_ports("1,65535") == [1, 65535]


@pytest.mark.parametrize(
    "spec",
    [
        "",            # empty
        "   ",         # whitespace only
        "22,,80",      # empty element
        "0",           # below range
        "65536",       # above range
        "100-50",      # low > high
        "http",        # not numeric
        "22-",         # missing high
        "-80",         # missing low
        "1-2-3",       # malformed range
    ],
)
def test_invalid_specs_rejected(spec):
    with pytest.raises(ProtocolError):
        parse_ports(spec)


def test_non_string_rejected():
    with pytest.raises(ProtocolError):
        parse_ports([22, 80])
