"""
ai_server.py
============
Falsight AI 서버 메인 진입점

현재 상태:
    ZMQ 수신부 주석 해제 시 실제 서버 모드로 전환

실행:
    python ai_server.py
"""

import cv2
import numpy as np
import logging
import struct
import multiprocessing as mp
import signal
import sys
import time
from config import (
    CAMERA_IDS, AI_RESOLUTION,
    ZMQ_HOST, ZMQ_PORT
)

# 로깅 설정
logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    handlers=[
        logging.StreamHandler(),
        logging.FileHandler("logs/ai_server.log", encoding="utf-8")
    ]
)
logger = logging.getLogger("AIServer")


# 더미 수신 프로세스 (C++ 연동 전 테스트용)
def dummy_receiver(frame_queues: dict):
    """
    C++ 연동 전 더미 프레임으로 내부 로직 테스트
    C++ 연동 후 zmq_receiver()로 교체
    """
    log = logging.getLogger("DummyReceiver")
    log.info("[Dummy] 더미 수신 시작 (C++ 연동 전 테스트 모드)")

    frame_id = 0
    while True:
        for camera_id in CAMERA_IDS:
            frame = np.random.randint(
                0, 50,
                (AI_RESOLUTION[1], AI_RESOLUTION[0], 3),
                dtype=np.uint8
            )
            meta = {
                "camera_id": camera_id,
                "timestamp": int(time.time() * 1000),
                "frame_id":  frame_id
            }
            try:
                frame_queues[camera_id].put_nowait((frame, meta))
            except Exception:
                pass
        frame_id += 1
        time.sleep(1 / 30)


# ZMQ 수신 프로세스 (C++ 연동 후 활성화)
def zmq_receiver(frame_queues: dict):
    """
    Node1 C++ 서버에서 ZMQ PUSH로 프레임 수신
    헤더 20바이트 파싱 후 camera_id에 맞는 Queue로 분배

    Node1 스펙:
        헤더: camera_id(1B) + padding(3B) + timestamp(8B)
              + frame_id(4B) + jpeg_size(4B) = 20바이트
        페이로드: JPEG 바이너리 (640×480, quality 85)
        camera_id: 0-indexed (0~3) → +1 해서 1-indexed로 변환

    bind/connect:
        AI 서버(bind) ← Node1(connect)
        AI 서버가 중심, Node1 여러 대 추가 시 코드 변경 없음
    """
    import zmq

    log = logging.getLogger("ZMQReceiver")

    context = zmq.Context()
    socket  = context.socket(zmq.PULL)

    # AI 서버가 bind → Node1들이 connect
    address = f"tcp://{ZMQ_HOST}:{ZMQ_PORT}"
    socket.bind(address)
    log.info(f"[ZMQ] 수신 대기 중: {address}")

    while True:
        try:
            parts = socket.recv_multipart()
            if len(parts) == 1:
                message = parts[0]  # 단일 메시지
            elif len(parts) == 2:
                message = parts[0] + parts[1]  # 헤더 + 페이로드 합치기
            else:
                continue
            #  검증 1단계: 최소 크기
            if len(message) < 20:
                log.warning("[ZMQ] 메시지 너무 짧음 → 스킵")
                continue

            # 헤더 파싱 (20바이트)
            camera_id = message[0]                                # 1B: 0~3
            # padding = message[1:4]                              # 3B: 무시
            timestamp = struct.unpack_from("<Q", message, 4)[0]  # 8B
            frame_id  = struct.unpack_from("<I", message, 12)[0] # 4B
            jpeg_size = struct.unpack_from("<I", message, 16)[0] # 4B

            # 검증 2단계: 페이로드 크기 일치
            payload = message[20:]
            if len(payload) != jpeg_size:
                log.warning(
                    f"[ZMQ] 크기 불일치 "
                    f"(기대={jpeg_size}, 실제={len(payload)}) → 스킵"
                )
                continue

            # 검증 3단계: JPEG 마커 확인
            if not (payload[:2] == b'\xff\xd8' and payload[-2:] == b'\xff\xd9'):
                log.warning("[ZMQ] JPEG 마커 불일치 → 스킵")
                continue

            # JPEG 디코딩
            nparr = np.frombuffer(payload, np.uint8)
            frame = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
            if frame is None:
                log.warning("[ZMQ] JPEG 디코딩 실패 → 스킵")
                continue

            # camera_id 보정 (0-indexed → 1-indexed)
            camera_id = int(camera_id) + 1  # 0~3 → 1~4

            meta = {
                "camera_id": camera_id,
                "timestamp": timestamp,
                "frame_id":  frame_id
            }

            if camera_id in frame_queues:
                try:
                    frame_queues[camera_id].put_nowait((frame, meta))
                except Exception:
                    pass  # Queue 가득 차면 최신 프레임 우선

        except Exception as e:
            log.error(f"[ZMQ] 수신 오류: {e}")
            continue


# 메인
def main():
    logger.info("=" * 50)
    logger.info("Falsight AI 서버 시작")
    logger.info("=" * 50)

    # 프로세스 간 Queue 생성
    frame_queues = {cam_id: mp.Queue(maxsize=30) for cam_id in CAMERA_IDS}
    result_queue = mp.Queue(maxsize=100)

    processes = []

    # 워커 프로세스 (카메라별 1개)
    from modules.worker_process import worker_process
    for cam_id in CAMERA_IDS:
        p = mp.Process(
            target=worker_process,
            args=(cam_id, frame_queues[cam_id], result_queue),
            name=f"Worker-cam{cam_id}",
            daemon=True
        )
        p.start()
        processes.append(p)
        logger.info(f"[Main] Worker cam{cam_id} 시작 (PID={p.pid})")

    # 알람 프로세스
    from modules.alarm_process import alarm_process
    alarm_p = mp.Process(
        target=alarm_process,
        args=(result_queue,),
        name="AlarmProcess",
        daemon=True
    )
    alarm_p.start()
    processes.append(alarm_p)
    logger.info(f"[Main] AlarmProcess 시작 (PID={alarm_p.pid})")

    # 수신 프로세스
    # C++ 연동 전: USE_DUMMY = True  (더미 프레임으로 테스트)
    # C++ 연동 후: USE_DUMMY = False (ZMQ 실제 수신)
    USE_DUMMY = False

    receiver_target = dummy_receiver if USE_DUMMY else zmq_receiver
    receiver_name   = "DummyReceiver" if USE_DUMMY else "ZMQReceiver"

    receiver_p = mp.Process(
        target=receiver_target,
        args=(frame_queues,),
        name=receiver_name,
        daemon=True
    )
    receiver_p.start()
    processes.append(receiver_p)
    logger.info(f"[Main] {receiver_name} 시작 (PID={receiver_p.pid})")

    # 종료 핸들러
    def shutdown(signum, frame):
        logger.info("[Main] 종료 신호 수신 → 프로세스 정리 중...")
        for cam_id in CAMERA_IDS:
            frame_queues[cam_id].put(None)
        result_queue.put(None)
        for p in processes:
            p.join(timeout=3)
            if p.is_alive():
                p.terminate()
        logger.info("[Main] AI 서버 종료 완료")
        sys.exit(0)

    signal.signal(signal.SIGINT,  shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    logger.info("[Main] 모든 프로세스 실행 중... (Ctrl+C 로 종료)")

    # 프로세스 감시
    while True:
        for p in processes:
            if not p.is_alive() and p.daemon:
                logger.warning(f"[Main] {p.name} 비정상 종료 감지")
        time.sleep(5)


if __name__ == "__main__":
    mp.set_start_method("spawn")
    main()