"""The SQLAlchemy instance, kept free of model definitions to avoid cycles."""

import sqlite3

from flask_sqlalchemy import SQLAlchemy
from sqlalchemy import event
from sqlalchemy.engine import Engine

db = SQLAlchemy()


@event.listens_for(Engine, "connect")
def _enable_sqlite_foreign_keys(dbapi_connection, connection_record):
    """Turn on foreign key enforcement for SQLite connections.

    SQLite ignores foreign keys by default. Retention prunes monitor_cycles
    with a bulk DELETE, which bypasses the ORM's cascade entirely, so without
    this pragma the child monitor_results rows would be orphaned rather than
    deleted. Guarded by an isinstance check so a non-SQLite engine is untouched.
    """
    if not isinstance(dbapi_connection, sqlite3.Connection):
        return
    cursor = dbapi_connection.cursor()
    try:
        cursor.execute("PRAGMA foreign_keys=1")
    finally:
        cursor.close()
