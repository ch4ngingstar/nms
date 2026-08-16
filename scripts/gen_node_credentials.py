"""Print broker credentials and an ACL block for one probe node.

Usage: python scripts/gen_node_credentials.py probe-a4c1f8
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from protocol.credentials import acl_block, generate_password  # noqa: E402
from protocol.errors import ProtocolError  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    node_id = argv[1]
    try:
        block = acl_block(node_id)
    except ProtocolError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    password = generate_password()
    print(f"# credentials for {node_id} - enter these in the captive portal")
    print(f"username: {node_id}")
    print(f"password: {password}")
    print()
    print("# add to the Mosquitto password file:")
    print(f"mosquitto_passwd -b /etc/mosquitto/passwd {node_id} {password}")
    print()
    print("# append to the Mosquitto ACL file:")
    print(block, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
