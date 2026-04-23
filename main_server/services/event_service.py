import httpx
import time
import os
import aiofiles
from datetime import datetime
from fastapi import UploadFile

from database.repository import FallEventRepository, VideoEvidenceRepository
from network.websocket_manager import ws_manager

NODE1_TCP_URL = "http://10.10.10.100:8080/api/edge/record"
UPLOAD_DIR = "./storage/video"
os.makedirs(UPLOAD_DIR, exist_ok=True)

class EventService:
    def __init__(self, fall_repo: FallEventRepository, video_repo: VideoEvidenceRepository = None):
        self.fall_repo = fall_repo
        self.video_repo = video_repo

    async def process_fall_event(self, camera_id: int, confidence: float, timestamp_str: str, request_start_time: float) -> int:
        try:
            timestamp_ms = int(timestamp_str)
        except ValueError:
            timestamp_ms = int(time.time() * 1000)

        # 1. Repository를 통해 DB 저장
        new_event_id = await self.fall_repo.save_fall_event(
            camera_id=camera_id,
            event_type=1,
            confidence=confidence,
            timestamp_ms=timestamp_ms
        )
        print(f"✅ [DB 저장 완료] 발급된 이벤트 ID: {new_event_id}")

        return new_event_id

    async def broadcast_alert(self, camera_id: int, event_id: int, request_start_time: float):
        alert_message = {
            "event": "FALL",
            "cam_id": camera_id,
            "fall_id": event_id
        }
        # 이미 싱글톤으로 생성된 ws_manager를 통해 브로드캐스트
        await ws_manager.broadcast(alert_message, request_start_time)

    async def send_record_command(self, fall_id: int, cam_id: int):
        async with httpx.AsyncClient() as client:
            try:
                payload = {
                    "fall_id": fall_id,
                    "cam_id": cam_id,
                    "duration": 15
                }
                response = await client.post(NODE1_TCP_URL, json=payload)
                print(f"✅ [녹화 명령 전송 성공] 엣지 응답 코드: {response.status_code}")
            except Exception as e:
                print(f"❌ [녹화 명령 전송 실패] 통신 에러: {e}")

    async def process_video_upload(
        self, 
        video_file: UploadFile, 
        fall_id: int, 
        camera_id: int, 
        clip_start: str, 
        clip_end: str
    ) -> tuple[str, int]:
        
        file_name = f"fall_{fall_id}_cam{camera_id}.mp4"
        physical_path = os.path.join(UPLOAD_DIR, file_name)
        virtual_path = f"/storage/video/{file_name}"

        # 비동기 파일 저장 (1MB chunk streaming)
        async with aiofiles.open(physical_path, 'wb') as out_file:
            while content := await video_file.read(1024 * 1024):
                await out_file.write(content)

        file_size = os.path.getsize(physical_path)

        # ISO 포맷 변환 (Z 제거 후 타임스탬프 계산)
        start_ms = int(datetime.fromisoformat(clip_start.replace('Z', '+00:00')).timestamp() * 1000)
        end_ms = int(datetime.fromisoformat(clip_end.replace('Z', '+00:00')).timestamp() * 1000)

        # DB 메타데이터 저장
        if self.video_repo:
            await self.video_repo.save_video_evidence(
                event_id=fall_id,
                start_ms=start_ms,
                end_ms=end_ms,
                file_path=virtual_path,
                file_size=file_size
            )
        
        return virtual_path, file_size

