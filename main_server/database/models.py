from sqlalchemy import Column, BigInteger, Float
from sqlalchemy.dialects.mysql import TINYINT
from .database import Base

class FallEvent(Base):
    __tablename__ = "FALL_EVENT"

    event_id = Column(BigInteger, primary_key=True, autoincrement=True)
    camera_id = Column(TINYINT(unsigned=True), nullable=False)
    event_type = Column(TINYINT(unsigned=True), nullable=False, default=1)
    confidence = Column(Float, nullable=True)
    timestamp_ms = Column(BigInteger, nullable=False)
