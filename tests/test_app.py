import pytest

from server.app import create_app


def test_factory_refuses_without_secret_key(monkeypatch):
    monkeypatch.delenv("SECRET_KEY", raising=False)
    with pytest.raises(RuntimeError, match="SECRET_KEY"):
        create_app({"SQLALCHEMY_DATABASE_URI": "sqlite:///:memory:"})


def test_factory_builds_with_a_secret_key():
    app = create_app({"SECRET_KEY": "x",
                      "SQLALCHEMY_DATABASE_URI": "sqlite:///:memory:"})
    assert app.blueprints["api"] is not None
    # Both the REST surface and the SSE stream register under /api.
    rules = {rule.rule for rule in app.url_map.iter_rules()}
    assert "/api/nodes" in rules
    assert "/api/stream" in rules
