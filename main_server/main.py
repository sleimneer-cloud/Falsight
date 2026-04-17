from fastapi import FastAPI, BackgroundTasks, WebSocket, WebSocketDisconnect
from pydantic import BaseModel
import httpx

app = FastAPI()

# 엣지 노드(Node 1) 정보 - 데이터를 '보낼' 목적지이므로 여기에 포트(8080)를 적어줍니다.
NODE1_IP = "10.10.10.100" # 엣지 담당자에게 받을 실제 IP로 나중에 수정하세요
NODE1_HTTP_PORT = 8080

# 1. AI 서버가 보낼 데이터의 형태
class FallEventRequest(BaseModel):
    event: str
    camera_id: int
    timestamp: str
    confidence: float


# 웹 소켓 연결 관리 매니저 : 여러명의 클라이언트가 동시에 접속할 수 있으니 리스트로 관리
class ConnectionManager:
    def __init__(self):
        self.active_connections: list[WebSocket] = []

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.append(websocket)

    def disconnect(self, websocket: WebSocket):
        self.active_connections.remove(websocket)

    async def broadcast(self, message: dict):
        for connection in self.active_connections:
            await connection.send_json(message)

manager = ConnectionManager()

# [추가된 기능] 웹 소켓 접속 엔드포인트
@app.websocket("/ws/client/alerts")
async def websocket_endpoint(websocket: WebSocket):
    await manager.connect(websocket)
    try:
        while True:
            # 클라이언트와의 연결을 유지하기 위해 무한 대기 (명세서의 heartbeat 역할)
            data = await websocket.receive_text()
    except WebSocketDisconnect:
        manager.disconnect(websocket)


# [추가된 기능] 엣지 노드에 녹화 명령을 보내는 비동기 함수
async def send_record_command(fall_id: int, cam_id: int):
    target_url = f"http://10.10.10.100:8080/api/edge/record"
    
    async with httpx.AsyncClient() as client:
        try:
            # 명세서 1-1에 맞춘 JSON 바디 구성
            payload = {
                "fall_id": fall_id,
                "cam_id": cam_id,
                "duration": 15
            }
            # 외부 서버로 POST 요청 쏘기
            response = await client.post(target_url, json=payload)
            print(f"✅ [녹화 명령 전송 성공] 엣지 응답 코드: {response.status_code}")
        except Exception as e:
            print(f"❌ [녹화 명령 전송 실패] 통신 에러: {e}")

# 2. AI 노드로부터 낙상 이벤트를 수신하는 엔드포인트
@app.post("/api/fall-event")
async def receive_fall_event(request_data: FallEventRequest, background_tasks: BackgroundTasks):
    print(f"🚨 [낙상 감지] 카메라 ID: {request_data.camera_id}, AI 신뢰도: {request_data.confidence * 100}%")
    
    new_event_id = 105 #아직 DB 연동이 안되어서 임의로 이벤트 ID를 할당함. 나중에 DB 연동 후 실제 ID로 수정예정...
    
    # 1. 엣지 카메라 녹화 지시 (백그라운드)
    background_tasks.add_task(send_record_command, fall_id=new_event_id, cam_id=request_data.camera_id)

    # 2. 클라이언트 앱에 긴급 알람 푸시 (백그라운드)
    
    alert_message = {
        "event": "FALL",
        "cam_id": request_data.camera_id,
        "fall_id": new_event_id
    }
    background_tasks.add_task(manager.broadcast, alert_message)

    # 3. AI 서버에 낙상 이벤트 수신 확인 응답
    return {"status": "received", "event_id": new_event_id}