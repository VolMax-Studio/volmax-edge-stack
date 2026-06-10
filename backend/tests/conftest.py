import os
import tempfile

os.environ["DATABASE_URL"] = "sqlite:///" + tempfile.mktemp(suffix=".db")

import pytest
from fastapi.testclient import TestClient

from app.db import Base, engine
from app.ingest import reset_detectors
from app.main import app


@pytest.fixture()
def client():
    Base.metadata.drop_all(engine)
    Base.metadata.create_all(engine)
    reset_detectors()
    with TestClient(app) as c:
        yield c
