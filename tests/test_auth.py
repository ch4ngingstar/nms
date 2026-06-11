import pytest
from flask import Flask, jsonify
from werkzeug.security import generate_password_hash

from server import auth

PASSWORD = "correct horse battery staple"


@pytest.fixture
def auth_app(monkeypatch):
    monkeypatch.setenv("NMS_ADMIN_PASSWORD_HASH", generate_password_hash(PASSWORD))
    application = Flask(__name__)
    application.config["SECRET_KEY"] = "test-secret"
    application.config["TESTING"] = True

    @application.post("/login")
    def login_route():
        from flask import request
        ok = auth.login(request.json["password"])
        return (jsonify({"ok": ok}), 200 if ok else 401)

    @application.post("/logout")
    def logout_route():
        auth.logout()
        return jsonify({"ok": True})

    @application.get("/guarded")
    @auth.require_auth
    def guarded():
        return jsonify({"secret": 42})

    return application


@pytest.fixture
def client(auth_app):
    return auth_app.test_client()


def test_correct_password_logs_in(client):
    assert client.post("/login", json={"password": PASSWORD}).status_code == 200


def test_wrong_password_is_rejected(client):
    assert client.post("/login", json={"password": "nope"}).status_code == 401


def test_guarded_route_blocks_anonymous(client):
    assert client.get("/guarded").status_code == 401


def test_guarded_route_allows_after_login(client):
    client.post("/login", json={"password": PASSWORD})
    response = client.get("/guarded")
    assert response.status_code == 200
    assert response.get_json()["secret"] == 42


def test_logout_revokes_access(client):
    client.post("/login", json={"password": PASSWORD})
    client.post("/logout")
    assert client.get("/guarded").status_code == 401


def test_login_fails_when_no_hash_configured(auth_app, monkeypatch):
    monkeypatch.delenv("NMS_ADMIN_PASSWORD_HASH", raising=False)
    client = auth_app.test_client()
    assert client.post("/login", json={"password": PASSWORD}).status_code == 401
