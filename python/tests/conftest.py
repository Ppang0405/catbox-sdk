import sys
from pathlib import Path

import pytest

# The mock catbox server is shared with the cross-language conformance suite.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "conformance"))
from mock_server import MockServer  # noqa: E402


@pytest.fixture(scope="session")
def server():
    server = MockServer()
    yield server
    server.close()


@pytest.fixture(autouse=True)
def reset_server(server):
    server.reset()
