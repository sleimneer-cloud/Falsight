import httpx
import time
from database.repository import FallEventRepository
from network.websocket_manager import ws_manager

NODE1_TCP_URL = "http://10.10.10.100:8080/api/edge/record"

class EventService:
    def __init__(self, fall_repo: FallEventRepository):
        self.fall_repo = fall_repo

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
