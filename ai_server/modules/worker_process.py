import cv2
import numpy as np
import logging
import multiprocessing as mp
from config import CAMERA_IDS

logger = logging.getLogger(__name__)


def worker_process(
    camera_id: int,
    frame_queue: mp.Queue,
    result_queue: mp.Queue
):
    """
    워커 프로세스 진입점

    입력:
        camera_id:    담당 카메라 번호
        frame_queue:  메인 프로세스로부터 프레임 수신
        result_queue: 판정 결과를 알람 프로세스로 전달
    """
    # 각 프로세스 독립 import (GIL 분리)
    from modules.keypoint_extractor import KeypointExtractor
    from modules.tracker            import SimpleTracker
    from modules.buffer_manager     import BufferManager
    from modules.fall_detector      import FallDetector, RESULT_FALL, RESULT_UNCERTAIN, RESULT_NON_FALL
    from modules.data_saver         import DataSaver
    from modules.debug_saver        import DebugSaver
    from config import DEBUG_SAVE_FRAMES

    # 로거 설정
    logging.basicConfig(level=logging.INFO)
    log = logging.getLogger(f"Worker-cam{camera_id}")
    log.info(f"[Worker cam{camera_id}] 프로세스 시작")

    # 모듈 초기화
    extractor  = KeypointExtractor()
    tracker    = SimpleTracker(max_lost=30)
    buffer_mgr = BufferManager()
    detector   = FallDetector()
    saver      = DataSaver()
    debug      = DebugSaver(camera_id, enabled=DEBUG_SAVE_FRAMES)

    while True:
        try:
            # 프레임 수신 (1초 대기)
            item = frame_queue.get(timeout=1.0)

            if item is None:
                # 종료 신호
                log.info(f"[Worker cam{camera_id}] 종료 신호 수신")
                break

            frame, frame_meta = item
            # frame_meta: {"camera_id": int, "timestamp": int, "frame_id": int}

            # 영상 버퍼에 프레임 추가 (자동 저장용)
            saver.add_frame(camera_id, frame)

            # 원본 프레임 저장 (디버그)
            debug.save_raw(frame)

            # Keypoint 추출
            detections, raw_det, kp_data, kp_conf = extractor.extract(frame)

            # 하체 감지 여부 계산 (디버그 오버레이용)
            lower_ok_flags = []
            if kp_data is not None and len(kp_data) > 0:
                for i in range(len(kp_data)):
                    lower_conf = kp_conf[i][[11, 12, 13, 14, 15, 16]].mean()
                    lower_ok_flags.append(lower_conf >= 0.3)

            # AI 시점 오버레이 저장 (디버그)
            debug.save_ai_view(frame, raw_det, kp_data, kp_conf, lower_ok_flags)

            # ByteTrack 추적
            tracks = tracker.update(detections)

            # 슬라이딩 윈도우 버퍼 + LSTM 추론
            for track in tracks:
                track_id  = track["track_id"]
                keypoints = track["keypoints"]

                # 버퍼에 추가 → 50프레임 찼으면 배열 반환
                buffer = buffer_mgr.update(track_id, keypoints)

                if buffer is None:
                    continue  # 아직 버퍼 부족

                # LSTM 추론
                result = detector.predict(buffer)

                if result is None:
                    continue  # 타임아웃 or 오류

                label      = result["label"]
                confidence = result["confidence"]

                log.debug(
                    f"[Worker cam{camera_id}] "
                    f"track={track_id} → {label} ({confidence:.3f})"
                )

                # 자동 저장 (FALL / UNCERTAIN)
                if label in [RESULT_FALL, RESULT_UNCERTAIN]:
                    saver.save(
                        label=label,
                        camera_id=camera_id,
                        track_id=track_id,
                        buffer=buffer,
                        confidence=confidence
                    )

                # FALL이면 Result Queue로 전달
                if label == RESULT_FALL:
                    result_queue.put({
                        "camera_id":  camera_id,
                        "track_id":   track_id,
                        "confidence": confidence,
                        "timestamp":  frame_meta.get("timestamp", 0)
                    })
                    log.info(
                        f"[Worker cam{camera_id}] "
                        f"🚨 FALL 감지! track={track_id} conf={confidence:.3f}"
                    )

        except mp.queues.Empty:
            # 프레임 없음 → 계속 대기
            continue
        except Exception as e:
            log.error(f"[Worker cam{camera_id}] 오류: {e}", exc_info=True)
            continue

    log.info(f"[Worker cam{camera_id}] 프로세스 종료")