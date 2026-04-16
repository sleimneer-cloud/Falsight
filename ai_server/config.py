"""
config.py
=========
Falsight AI 서버 전체 설정값 중앙 관리

모델 버전 전환:
    MODEL_VERSION = 1  ->  MediaPipe 33관절 (99 피처)
    MODEL_VERSION = 2  ->  YOLO11 Pose 17관절 (34 피처)
"""

# ── 모델 버전 ─────────────────────────────────────────────────
MODEL_VERSION = 1       # 1 = MediaPipe / 2 = YOLO11 Pose

# 기본값 선언 (IDE 경고 방지, 아래 if 블록에서 덮어씌워짐)
N_FEATURES    = 99
MODEL_PATH    = "models/fall_model_v1.keras"
USE_MEDIAPIPE = True

if MODEL_VERSION == 1:
    N_FEATURES    = 99
    MODEL_PATH    = "models/fall_model_v1.keras"
    USE_MEDIAPIPE = True
elif MODEL_VERSION == 2:
    N_FEATURES    = 34
    MODEL_PATH    = "models/fall_model_v2.keras"
    USE_MEDIAPIPE = False

# ── 실행 환경 ─────────────────────────────────────────────────
USE_GPU = False   # CPU: False / GPU(RTX 3060): True

# ── 입력 shape ────────────────────────────────────────────────
FRAME_WINDOW  = 100     # 슬라이딩 윈도우 프레임 수

# ── 판정 임계값 ───────────────────────────────────────────────
FALL_THRESHOLD      = 0.80   # 이 값 이상이면 FALL
UNCERTAIN_MIN       = 0.60   # UNCERTAIN 구간 하한
UNCERTAIN_MAX       = 0.80   # UNCERTAIN 구간 상한
                              # 0.60 미만 -> NON-FALL (폐기)

# ── 카메라 설정 ───────────────────────────────────────────────
CAMERA_COUNT  = 4            # 연결된 카메라 수
CAMERA_IDS    = [1, 2, 3, 4] # 1-indexed (ZMQ 수신 시 0-indexed -> +1 변환)

# ── 영상 해상도 (Node1 확정 스펙) ────────────────────────────
AI_RESOLUTION = (640, 480)   # Node1 전송 해상도 확정값

# ── 타임아웃 ─────────────────────────────────────────────────
INFERENCE_TIMEOUT_MS = 150   # 추론 제한 시간 (ms)

# ── 알람 중복 방지 ────────────────────────────────────────────
ALARM_COOLDOWN_SEC = 30      # 동일 camera_id 재알람 대기 시간

# ── ZMQ 수신 ─────────────────────────────────────────────────
# AI 서버가 bind -> Node1이 connect
ZMQ_HOST = "0.0.0.0"
ZMQ_PORT = 9001

# ── Node 3 (메인 서버) HTTP POST ──────────────────────────────
NODE3_HOST = "192.168.0.100"   # Node3 실제 IP로 변경
NODE3_PORT = 8000
NODE3_FALL_ENDPOINT = f"http://{NODE3_HOST}:{NODE3_PORT}/api/fall-event"
NODE3_RETRY_COUNT   = 3        # 전송 실패 시 재시도 횟수

# ── 자동 저장 경로 ────────────────────────────────────────────
SAVE_DIR_RAW        = "data/test_collected/raw"
SAVE_DIR_FALL       = "data/test_collected/labeled/fall"
SAVE_DIR_UNCERTAIN  = "data/test_collected/labeled/uncertain"
SAVE_DIR_NON_FALL   = "data/test_collected/labeled/non_fall"
SAVE_DIR_RETRAIN    = "data/test_collected/retrain_ready"

# ── SQLite (재학습 데이터 관리) ───────────────────────────────
# Python 내장 모듈, 별도 설치 불필요
# 서버 PC 이전 시 db 파일만 복사하면 됨
SQLITE_DB_PATH = "data/falsight_ai.db"

# ── 로그 경로 ─────────────────────────────────────────────────
LOG_DIR  = "logs"
LOG_FILE = "logs/ai_server.log"