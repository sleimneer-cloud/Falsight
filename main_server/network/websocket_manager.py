from fastapi import WebSocket
import time

class ConnectionManager:
    def __init__(self):
        self.active_connections: list[WebSocket] = []

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.append(websocket)

    def disconnect(self, websocket: WebSocket):
        if websocket in self.active_connections:
            self.active_connections.remove(websocket)

    async def broadcast(self, message: dict, start_time: float = None):
        for connection in self.active_connections:
            try:
                await connection.send_json(message)
                if start_time:
                    elapsed_ms = (time.perf_counter() - start_time) * 1000
                    print(f"✅ [웹소켓] 낙상 알람 전송 성공 ({elapsed_ms:.2f} ms 소요): {message}")
                else:
                    print(f"✅ [웹소켓] 낙상 알람 전송 성공: {message}")
            except Exception as e:
                print(f"❌ [웹소켓] 낙상 알람 전송 실패: {e}")

# 싱글톤 객체: 프로젝트 전역에서 동일한 매니저 상태를 공유하기 위해 사용합니다.
ws_manager = ConnectionManager()
