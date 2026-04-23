from fastapi import APIRouter, WebSocket, WebSocketDisconnect
from network.websocket_manager import ws_manager

router = APIRouter()

@router.websocket("/ws/client/alerts")
async def websocket_endpoint(websocket: WebSocket):
    client_ip = websocket.client.host if websocket.client else 'Unknown'
    print(f"🔔 [웹소켓] 클라이언트 연결 요청 수신 (IP: {client_ip})")
    
    try:
        await ws_manager.connect(websocket)
        print(f"✅ [웹소켓] 클라이언트 연결 성공 (현재 접속자 수: {len(ws_manager.active_connections)})")
        while True:
            # 클라이언트와의 연결을 유지하기 위해 무한 대기 (명세서의 heartbeat 역할)
            data = await websocket.receive_text()
    except WebSocketDisconnect:
        ws_manager.disconnect(websocket)
        print(f"⚠️ [웹소켓] 클라이언트 연결 정상 종료 (현재 접속자 수: {len(ws_manager.active_connections)})")
    except Exception as e:
        ws_manager.disconnect(websocket)
        print(f"❌ [웹소켓] 클라이언트 접속/통신 중 예외 발생: {e}")
