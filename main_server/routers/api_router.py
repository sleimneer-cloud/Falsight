import time
from fastapi import APIRouter, BackgroundTasks, Depends
from sqlalchemy.ext.asyncio import AsyncSession

from schemas.fall import FallEventRequest
from database.database import get_db
from database.repository import FallEventRepository
from services.event_service import EventService

router = APIRouter()

# Dependency Injection (DI) 체인: Session -> Repository -> Service
def get_event_service(db: AsyncSession = Depends(get_db)) -> EventService:
    repo = FallEventRepository(db)
    return EventService(repo)

@router.post("/api/fall-event")
async def receive_fall_event(
    request_data: FallEventRequest, 
    background_tasks: BackgroundTasks,
    service: EventService = Depends(get_event_service)
):
    request_start_time = time.perf_counter() # 낙상 신호 수신 즉시 타이머 시작
    print(f"🚨 [낙상 감지] 카메라 ID: {request_data.camera_id}, AI 신뢰도: {request_data.confidence * 100}%")
    
    # 1. 서비스 로직 호출 (DB 저장 등 모든 비즈니스 로직 위임)
    new_event_id = await service.process_fall_event(
        camera_id=request_data.camera_id,
        confidence=request_data.confidence,
        timestamp_str=request_data.timestamp,
        request_start_time=request_start_time
    )
    
    # 2. 백그라운드 태스크 할당 (웹소켓 알림 및 엣지 노드 녹화명령 제어)
    background_tasks.add_task(service.broadcast_alert, request_data.camera_id, new_event_id, request_start_time)
    background_tasks.add_task(service.send_record_command, new_event_id, request_data.camera_id)

    return {"status": "received", "event_id": new_event_id}
