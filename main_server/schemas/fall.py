from pydantic import BaseModel

class FallEventRequest(BaseModel):
    event: str
    camera_id: int
    timestamp: str
    confidence: float
