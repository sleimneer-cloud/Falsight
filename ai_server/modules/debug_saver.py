"""
modules/debug_saver.py
======================
개발/디버깅용 프레임 저장 모듈

저장 내용:
    1. Node1에서 수신한 원본 프레임 (raw)
    2. YOLO BBox + 관절 시각화 오버레이 프레임 (ai_view)

특징:
    - 카메라별 독립 폴더로 저장
    - config.py DEBUG_SAVE_FRAMES = True 일 때만 동작
    - 운영 환경에서는 False로 비활성화

저장 경로:
    debug/
    └── cam1/
        ├── raw/       ← Node1 수신 원본
        └── ai_view/   ← YOLO 오버레이 적용본
    └── cam2/
        ...
"""

import os
import cv2
import numpy as np
import logging
from datetime import datetime

logger = logging.getLogger(__name__)

# COCO 17관절 연결 구조 (시각화용 skeleton)
SKELETON_EDGES = [
    (0, 1), (0, 2),             # 코 → 눈
    (1, 3), (2, 4),             # 눈 → 귀
    (5, 6),                     # 어깨 연결
    (5, 7), (7, 9),             # 왼팔
    (6, 8), (8, 10),            # 오른팔
    (5, 11), (6, 12),           # 어깨 → 엉덩이
    (11, 12),                   # 엉덩이 연결
    (11, 13), (13, 15),         # 왼다리
    (12, 14), (14, 16),         # 오른다리
]

# 관절 색상 (상체=파랑, 하체=초록)
KEYPOINT_COLOR = (0, 200, 255)      # 관절 점
SKELETON_COLOR = (0, 255, 128)      # 뼈대 선
BBOX_COLOR     = (255, 100, 0)      # BBox
LOWER_COLOR    = (0, 255, 0)        # 하체 감지됨
SKIP_COLOR     = (0, 0, 255)        # 하체 미감지 (스킵)

DEBUG_BASE_DIR = "debug"


class DebugSaver:
    """
    디버그용 프레임 저장

    사용 예시:
        saver = DebugSaver(camera_id=1)
        saver.save_raw(frame)
        saver.save_ai_view(frame, detections, keypoints_data, keypoints_conf, lower_ok)
    """

    def __init__(self, camera_id: int, enabled: bool = True):
        self.camera_id = camera_id
        self.enabled   = enabled
        self.frame_idx = 0

        if self.enabled:
            self._ensure_dirs()
            logger.info(
                f"[Debug] cam{camera_id} 디버그 저장 활성화 "
                f"→ {DEBUG_BASE_DIR}/cam{camera_id}/"
            )
        else:
            logger.info(f"[Debug] cam{camera_id} 디버그 저장 비활성화")

    def _ensure_dirs(self):
        for sub in ["raw", "ai_view"]:
            path = self._dir(sub)
            os.makedirs(path, exist_ok=True)

    def _dir(self, sub: str) -> str:
        return os.path.join(DEBUG_BASE_DIR, f"cam{self.camera_id}", sub)

    def _filename(self, sub: str) -> str:
        ts = datetime.now().strftime("%H%M%S_%f")[:-3]
        return os.path.join(
            self._dir(sub),
            f"{ts}_f{self.frame_idx:05d}.jpg"
        )

    # 공개 메서드

    def save_raw(self, frame: np.ndarray):
        """
        Node1에서 수신한 원본 프레임 저장
        """
        if not self.enabled:
            return
        try:
            path = self._filename("raw")
            cv2.imwrite(path, frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
            logger.debug(f"[Debug] raw 저장: {path}")
        except Exception as e:
            logger.error(f"[Debug] raw 저장 실패: {e}")

    def save_ai_view(
        self,
        frame: np.ndarray,
        detections,
        keypoints_data: np.ndarray,
        keypoints_conf: np.ndarray,
        lower_ok_flags: list[bool]
    ):
        """
        YOLO BBox + 관절 시각화 오버레이 후 저장 (AI가 보는 시점)

        입력:
            frame:           원본 프레임
            detections:      YOLO 결과 객체
            keypoints_data:  (N, 17, 2) 관절 좌표
            keypoints_conf:  (N, 17)    관절 confidence
            lower_ok_flags:  [True/False, ...]  하체 감지 여부 (사람별)
        """
        if not self.enabled:
            self.frame_idx += 1
            return

        try:
            vis = frame.copy()

            boxes = detections.boxes if detections is not None else None

            for i, kp_17 in enumerate(keypoints_data):
                lower_ok = lower_ok_flags[i] if i < len(lower_ok_flags) else False

                # BBox
                if boxes is not None and i < len(boxes):
                    x1, y1, x2, y2 = map(int, boxes.xyxy[i].tolist())
                    color = LOWER_COLOR if lower_ok else SKIP_COLOR
                    cv2.rectangle(vis, (x1, y1), (x2, y2), color, 2)

                    # 하체 감지 여부 텍스트
                    label = "FULL" if lower_ok else "SKIP(lower)"
                    cv2.putText(
                        vis, label, (x1, y1 - 8),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1
                    )

                # 관절 점
                for j, (x, y) in enumerate(kp_17):
                    if x == 0 and y == 0:
                        continue
                    conf = float(keypoints_conf[i][j]) if keypoints_conf is not None else 1.0
                    if conf < 0.2:
                        continue
                    radius = max(2, int(conf * 5))
                    cv2.circle(vis, (int(x), int(y)), radius, KEYPOINT_COLOR, -1)

                # 뼈대 선
                for a, b in SKELETON_EDGES:
                    xa, ya = kp_17[a]
                    xb, yb = kp_17[b]
                    if xa == 0 and ya == 0:
                        continue
                    if xb == 0 and yb == 0:
                        continue
                    ca = float(keypoints_conf[i][a]) if keypoints_conf is not None else 1.0
                    cb = float(keypoints_conf[i][b]) if keypoints_conf is not None else 1.0
                    if ca < 0.2 or cb < 0.2:
                        continue
                    cv2.line(
                        vis,
                        (int(xa), int(ya)),
                        (int(xb), int(yb)),
                        SKELETON_COLOR, 1
                    )

            # 카메라 정보 워터마크
            cv2.putText(
                vis,
                f"CAM{self.camera_id} | f{self.frame_idx:05d}",
                (8, 20),
                cv2.FONT_HERSHEY_SIMPLEX, 0.5,
                (200, 200, 200), 1
            )

            path = self._filename("ai_view")
            cv2.imwrite(path, vis, [cv2.IMWRITE_JPEG_QUALITY, 85])
            logger.debug(f"[Debug] ai_view 저장: {path}")

        except Exception as e:
            logger.error(f"[Debug] ai_view 저장 실패: {e}")
        finally:
            self.frame_idx += 1
