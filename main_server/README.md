# FalSight Main Server 🚨

FalSight 메인 서버는 엣지 디바이스(AI 카메라 노드)로부터 낙상 감지 이벤트를 실시간으로 수신하고, 관제 클라이언트(앱/웹)로 알림을 브로드캐스트하며, 실시간 영상 및 데이터를 안정적으로 처리하기 위한 **비동기 기반 멀티 프로토콜(HTTP & TCP) 백엔드 서버**입니다.

---

## 🏗 Architecture Overview (아키텍처 개요)

이 프로젝트는 유지보수성과 확장성을 극대화하기 위해 **관심사 분리(Separation of Concerns, SoC)** 원칙에 입각한 **3-Tier Layered Architecture** 상에서 구축되었습니다. 

단일 스레드 비동기 이벤트 루프(`asyncio`) 위에서 FastAPI 웹 서버와 Custom TCP 소켓 서버가 자원 충돌 없이 완벽한 병렬로 동작합니다.

### 📂 Directory Structure
```text
main_server/
├── main.py                 # Application 진입점 및 Lifespan 초기화
├── routers/                # 1. API 라우터 계층 (엔드포인트)
│   ├── api_router.py       # - RESTful HTTP API (낙상 이벤트 수신 등)
│   └── ws_router.py        # - WebSocket 통신 (클라이언트 실시간 알림)
├── services/               # 2. 비즈니스 로직 계층 (유스케이스)
│   └── event_service.py    # - 낙상 판단, 알람 지시, 엣지 노드 제어, 데이터 정제
├── database/               # 3. 데이터 및 영속성 계층 (DB I/O)
│   ├── database.py         # - 커넥션 풀(AsyncSession) 및 엔진 설정
│   ├── models.py           # - SQLAlchemy ORM 데이터베이스 모델
│   └── repository.py       # - DB 트랜잭션을 전담하는 Repository 클래스
├── network/                # 4. 네트워크 통신 제어 계층
│   ├── tcp_server.py       # - 8100 포트 C/C++ 하드웨어 커스텀 패킷 처리(struct)
│   └── websocket_manager.py# - 1:N 클라이언트 WebSocket 연결 스레드 세이프 관리 (Singleton)
├── schemas/                # 5. 데이터 검증 계층
│   └── fall.py             # - Pydantic 기반 Request/Response 데이터 타입 스키마
├── README.md               # 프로젝트 설명서
└── requirements.txt        # 의존성 패키지 목록
```

---

## 🔌 주요 기능 및 네트워크 통신 프로토콜

본 서버는 하나의 애플리케이션 내에서 목적이 다른 두 개의 네트워크망을 동시에 운영합니다.

### 1. HTTP 웹 서버 & 웹소켓 (Port : `8000`) - 프론트엔드 및 AI 연동
* **`FASTAPI` & `Uvicorn`** 기반
* **POST `/api/fall-event`**: AI 엣지 노드로부터 실시간 낙상(Fall) 감지 이벤트를 수신합니다. DB에 기록을 남긴 뒤, 서비스 레이어를 통해 연관 로직을 비동기로 백그라운드에서 실행합니다.
* **WS `/ws/client/alerts`**: 관제 앱/웹과 연결을 지속하며, 낙상 발생 시 즉각적으로 푸시 알림(Payload)을 브로드캐스트합니다.

### 2. Custom TCP 소켓 서버 (Port : `8100`) - 디바이스 및 C/C++ 연동
* **순수 `asyncio.start_server`** 기반
* HTTP 오버헤드가 부담스러운 엣지 기기나 C/C++ 하드웨어 클라이언트와의 다이렉트 통신을 전담합니다.
* 20바이트 고정길이 바이너리 헤더를 읽어(`reader.readexactly`), `struct.unpack` 방식으로 안전하게 파싱한 후 `Message ID` 베이스 라우팅을 수행합니다. 영상 데이터 요청(310, 302 등)에 대한 고속 바이너리 통신을 지원합니다.

---

## 🛡 Design Patterns (디자인 패턴 활용)

1. **Dependency Injection (의존성 주입)**
   모든 Router는 직접 DB나 Service 클래스를 생성하지 않고 FastAPI의 `Depends`를 통해 주입받습니다. 코드의 복잡성이 낮아지고 Mock 테스트 시 유연하게 대처할 수 있습니다.
2. **Repository Pattern (저장소 패턴)**
   개발자가 직접 쿼리(`session.execute`)를 날리지 않고, `FallEventRepository`와 같은 역할 클래스에 위임하여 DB 계층과 핵심 서비스 로직의 결합도를 낮췄습니다.
3. **Singleton (싱글톤 패턴)**
   WebSocket `ConnectionManager`는 메모리 주소를 공유해야 하므로 서버 생명주기 동안 유일한 객체로 인스턴스화되어 있습니다.

---

## 🛠 Tech Stack (기술 스택)

- **Language:** Python 3.10+
- **Framework:** FastAPI, Uvicorn
- **Async I/O:** `asyncio` (Native coroutines), `struct` (Binary parsing)
- **Database:** MariaDB (or MySQL)
- **ORM & Driver:** SQLAlchemy 2.0 (Async), `asyncmy`
- **Validation:** Pydantic

---

## 🚀 Getting Started (설행 방법)

### 1. 패키지 설치
Python 가상환경(venv 등)을 활성화한 후 의존성 패키지를 설치합니다.
```bash
pip install -r requirements.txt
```

### 2. 환경 변수 설정
프로젝트 루트 디렉토리에 `.env` 파일을 생성하고 아래 구조에 맞게 작성합니다.
```env
DB_USER=root
DB_PASSWORD=your_password
DB_HOST=127.0.0.1
DB_PORT=3306
DB_NAME=falsight_db
SERVER_HOST_IP=127.0.0.1
```

### 3. 서버 실행
FastAPI 개발(또는 프로덕션) 서버를 가동합니다. 구동과 동시에 DB 연결 풀링 확인 및 8100번 TCP 포트 개방 과정이 로그에 표시됩니다.
```bash
uvicorn main:app --reload
```
