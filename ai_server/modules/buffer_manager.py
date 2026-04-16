"""
modules/buffer_manager.py
=========================
슬라이딩 윈도우 버퍼 관리 모듈

역할:
    - track_id별 독립적인 100프레임 버퍼 유지
    - 버퍼가 100프레임 찼을 때 CNN 추론용 배열 반환
    - track_id 소실 시 해당 버퍼 초기화
    - 연결 끊김 시 전체 초기화
"""

import numpy as np
import logging
from collections import deque
from config import FRAME_WINDOW, N_FEATURES

logger = logging.getLogger(__name__)


class BufferManager:
    """
    track_id별 슬라이딩 윈도우 버퍼

    사용 예시:
        bm = BufferManager()
        ready = bm.update(track_id=1, keypoints=kp_array)
        if ready is not None:
            # ready: shape (FRAME_WINDOW, N_FEATURES) → CNN 입력
    """

    def __init__(self):
        # {track_id: deque(maxlen=FRAME_WINDOW)}
        self.buffers: dict[int, deque] = {}

    def update(self, track_id: int, keypoints: np.ndarray) -> np.ndarray | None:
        """
        track_id의 버퍼에 keypoints 추가

        반환:
            버퍼가 FRAME_WINDOW 찼으면 → (FRAME_WINDOW, N_FEATURES) 배열
            아직 부족하면              → None
        """
        if keypoints is None:
            # 추적은 되지만 keypoint 없는 경우 (정지 감지 없는 프레임)
            return None

        if len(keypoints) != N_FEATURES:
            logger.warning(
                f"[Buffer] track_id={track_id} 피처 수 불일치: "
                f"기대={N_FEATURES}, 실제={len(keypoints)}"
            )
            return None

        # 버퍼 없으면 생성
        if track_id not in self.buffers:
            self.buffers[track_id] = deque(maxlen=FRAME_WINDOW)
            logger.debug(f"[Buffer] track_id={track_id} 버퍼 생성")

        self.buffers[track_id].append(keypoints.copy())

        # 버퍼 가득 찼으면 추론용 배열 반환
        if len(self.buffers[track_id]) == FRAME_WINDOW:
            return np.array(self.buffers[track_id], dtype=np.float32)  # (100, N)

        # 현재 버퍼 상태 로그 (10프레임마다)
        current = len(self.buffers[track_id])
        if current % 10 == 0:
            logger.debug(
                f"[Buffer] track_id={track_id} 버퍼: {current}/{FRAME_WINDOW}"
            )

        return None

    def remove(self, track_id: int):
        """track_id 추적 종료 시 버퍼 삭제"""
        if track_id in self.buffers:
            del self.buffers[track_id]
            logger.debug(f"[Buffer] track_id={track_id} 버퍼 삭제")

    def reset(self):
        """카메라 연결 끊김 시 전체 초기화"""
        self.buffers.clear()
        logger.info("[Buffer] 전체 버퍼 초기화")

    def get_status(self) -> dict:
        """현재 버퍼 상태 반환 (디버깅용)"""
        return {
            tid: f"{len(buf)}/{FRAME_WINDOW}"
            for tid, buf in self.buffers.items()
        }
