from sqlalchemy.ext.asyncio import AsyncSession
from sqlalchemy import text
from database.models import FallEvent

class FallEventRepository:
    def __init__(self, session: AsyncSession):
        self.session = session

    async def save_fall_event(self, camera_id: int, event_type: int, confidence: float, timestamp_ms: int) -> int:
        new_event = FallEvent(
            camera_id=camera_id,
            event_type=event_type,
            confidence=confidence,
            timestamp_ms=timestamp_ms
        )
        self.session.add(new_event)
        await self.session.commit()
        await self.session.refresh(new_event)
        return new_event.event_id

class VideoEvidenceRepository:
    def __init__(self, session: AsyncSession):
        self.session = session

    async def get_video_by_event_id(self, event_id: int) -> str | None:
        sql = text("""
            SELECT file_path 
            FROM VIDEO_EVIDENCE 
            WHERE event_id = :evt_id
        """)
        result = await self.session.execute(sql, {"evt_id": event_id})
        row = result.fetchone()
        if row:
            return row.file_path
        return None

    async def get_videos_by_time_range(self, cam_id: int, start_time: int, end_time: int):
        # cam_id가 0xFF(255)이면 전체 카메라 조회
        sql = text("""
            SELECT event_id, camera_id, timestamp_ms 
            FROM FALL_EVENT 
            WHERE (camera_id = :cam_id OR :cam_id = 255)
              AND timestamp_ms BETWEEN :start AND :end
            ORDER BY timestamp_ms DESC
        """)
        result = await self.session.execute(sql, {"cam_id": cam_id, "start": start_time, "end": end_time})
        return result.fetchall()

    async def save_video_evidence(self, event_id: int, start_ms: int, end_ms: int, file_path: str, file_size: int):
        sql = text("""
            INSERT INTO VIDEO_EVIDENCE 
            (event_id, clip_start_ms, clip_end_ms, file_path, file_size) 
            VALUES (:event_id, :start, :end, :path, :size)
        """)
        await self.session.execute(sql, {
            "event_id": event_id,
            "start": start_ms,
            "end": end_ms,
            "path": file_path,
            "size": file_size
        })
        await self.session.commit()

