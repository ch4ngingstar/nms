import pytest
from flask import Flask

from server.db import db as _db


@pytest.fixture
def app():
    application = Flask(__name__)
    application.config["SQLALCHEMY_DATABASE_URI"] = "sqlite:///:memory:"
    application.config["TESTING"] = True
    _db.init_app(application)
    with application.app_context():
        _db.create_all()
        yield application
        _db.session.remove()
        _db.drop_all()


@pytest.fixture
def db(app):
    return _db


@pytest.fixture
def node(db):
    """A registered node, since almost everything is foreign-keyed to one."""
    from server.models import Node

    record = Node(node_id="probe-a4c1f8", label="Lab North", fw="1.2.0",
                  chip="esp32s3", mac="a0:b7:65:a4:c1:f8",
                  capabilities=["port_scan", "wifi_survey"], state="online")
    db.session.add(record)
    db.session.commit()
    return record


@pytest.fixture
def device(db):
    from server.models import Device

    record = Device(name="Main Router", ip="192.168.1.1", role="router")
    db.session.add(record)
    db.session.commit()
    return record
