import sqlite3

from flask import Flask

from server.db import db


def test_sqlite_foreign_keys_are_enforced():
    """SQLite ignores foreign keys unless asked, and retention relies on them."""
    app = Flask(__name__)
    app.config["SQLALCHEMY_DATABASE_URI"] = "sqlite:///:memory:"
    db.init_app(app)
    with app.app_context():
        raw = db.session.connection().connection.driver_connection
        assert isinstance(raw, sqlite3.Connection)
        enabled = raw.execute("PRAGMA foreign_keys").fetchone()[0]
        assert enabled == 1, "foreign key enforcement is off"
