import os
from dotenv import load_dotenv
from sqlalchemy.ext.asyncio import create_async_engine, AsyncSession
from sqlalchemy.orm import sessionmaker, declarative_base

# ==========================================
# 0. 환경 변수(.env) 로드
# ==========================================
load_dotenv()

# 환경 변수에서 데이터베이스 접속 정보 가져오기 (기본값 설정됨)
DB_USER = os.getenv("DB_USER")
DB_PASSWORD = os.getenv("DB_PASSWORD")
DB_HOST = os.getenv("DB_HOST")
DB_PORT = os.getenv("DB_PORT")
DB_NAME = os.getenv("DB_NAME")


if not DB_PASSWORD:
    raise ValueError("환경 변수에 DB_PASSWORD가 설정되지 않았습니다.")
if not DB_USER:
    raise ValueError("환경 변수에 DB_USER가 설정되지 않았습니다.")
if not DB_HOST:
    raise ValueError("환경 변수에 DB_HOST가 설정되지 않았습니다.")
if not DB_PORT:
    raise ValueError("환경 변수에 DB_PORT가 설정되지 않았습니다.")
if not DB_NAME:
    raise ValueError("환경 변수에 DB_NAME가 설정되지 않았습니다.")

# ==========================================
# 1. DB 접속 URL 설정
# ==========================================
DATABASE_URL = f"mysql+asyncmy://{DB_USER}:{DB_PASSWORD}@{DB_HOST}:{DB_PORT}/{DB_NAME}"

# ==========================================
# 2. 비동기 엔진 및 커넥션 풀 생성
# ==========================================
engine = create_async_engine(
    DATABASE_URL,
    echo=False,                   # 터미널에 실행된 SQL 쿼리 로그를 출력할지 여부 (개발 시에는 True 권장)
    pool_size=20,                 # 기본 커넥션 풀 크기 (유지할 기본 커넥션 개수)
    max_overflow=10,              # 풀이 꽉 찼을 때 추가로 생성할 수 있는 최대 커넥션 수
    pool_timeout=30,              # 풀에서 커넥션을 얻기 위해 대기하는 최대 시간 (초)
    pool_recycle=1800,            # DB 연결이 끊어지는 것을 방지하기 위해 30분(1800초)마다 커넥션을 갱신
)

# ==========================================
# 3. 비동기 데이터베이스 세션 팩토리 생성
# ==========================================
AsyncSessionLocal = sessionmaker(
    bind=engine,
    class_=AsyncSession,
    autocommit=False,
    autoflush=False,
    expire_on_commit=False,
)

# SQLAlchemy ORM 모델을 정의할 때 상속받을 Base 클래스
Base = declarative_base()

# ==========================================
# 4. Dependency 인젝션용 세션 제너레이터 
# FastAPI의 Depends()를 이용해 각 API 엔드포인트에 DB 세션을 주입합니다.
# ==========================================
async def get_db():
    async with AsyncSessionLocal() as session:
        try:
            yield session
        finally:
            await session.close()
