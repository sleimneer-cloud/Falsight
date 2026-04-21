import asyncio
import os
from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles
from contextlib import asynccontextmanager

from database.database import engine
from network.tcp_server import handle_custom_tcp_client
from routers import api_router, ws_router

@asynccontextmanager
async def lifespan(app: FastAPI):
    # 1. DB 초기화/테스트
    try:
        async with engine.begin() as conn:
            print("✅ [DB 연결 성공] 데이터베이스 커넥션 풀이 자동 생성되고 성공적으로 연결되었습니다.")
    except Exception as e:
        print(f"❌ [DB 연결 오류] 데이터베이스 연결에 실패했습니다: {e}")

    # 2. TCP 서버(8100번 포트) 개방 (백그라운드)
    tcp_server = await asyncio.start_server(handle_custom_tcp_client, '0.0.0.0', 8100)
    print("✅ [TCP 서버] 8100번 포트 개방 완료 (Custom Binary Protocol 대기 중)")
    
    yield # FastAPI 8000번 포트 웹 서버 시작
    
    # 3. 종료 시 자원 회수
    tcp_server.close()
    await tcp_server.wait_closed()
    print("⚠️  [TCP 서버] 8100번 포트 닫힘")
    await engine.dispose()
    print("⚠️  [DB 연결 종료] 안전하게 데이터베이스 커넥션 풀이 해제되었습니다.")

app = FastAPI(lifespan=lifespan)

# [중요] Video 물리적 경로 생성 및 웹 정적 파일 제공 경로 마운트
os.makedirs("./storage", exist_ok=True)
app.mount("/storage", StaticFiles(directory="./storage"), name="storage")

# 라우터 등록 (HTTP / WebSocket 분리)
app.include_router(api_router.router)
app.include_router(ws_router.router)