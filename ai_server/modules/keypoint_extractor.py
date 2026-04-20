"""
modules/keypoint_extractor.py
=============================
YOLO11 Pose 기반 Keypoint 추출 모듈

추출 방식: YOLO11 Pose 단일 forward pass
관절 수:   COCO 17관절 × (x, y) = 34피처

전신 필터:
    하체(엉덩이~발목) confidence 평균 < 0.3 → 스킵
    전신이 보이지 않는 경우 오탐 방지

반환값:
    (results, detections, keypoints_data, keypoints_conf)
    results:        추론에 사용할 keypoint 리스트
    detections:     YOLO raw 결과 (디버그 시각화용)
    keypoints_data: (N, 17, 2) 전체 관절 좌표 (디버그용)
    keypoints_conf: (N, 17)    전체 관절 confidence (디버그용)
"""

import cv2
import numpy as np
import logging
from config import N_FEATURES, AI_RESOLUTION

logger = logging.getLogger(__name__)

# COCO 17관절 순서
COCO_KEYPOINTS = [
    'Nose',           # 0
    'Left Eye',       # 1
    'Right Eye',      # 2
    'Left Ear',       # 3
    'Right Ear',      # 4
    'Left Shoulder',  # 5
    'Right Shoulder', # 6
    'Left Elbow',     # 7
    'Right Elbow',    # 8
    'Left Wrist',     # 9
    'Right Wrist',    # 10
    'Left Hip',       # 11
    'Right Hip',      # 12
    'Left Knee',      # 13
    'Right Knee',     # 14
    'Left Ankle',     # 15
    'Right Ankle'     # 16
]

# 하체 관절 인덱스 (엉덩이 ~ 발목)
LOWER_BODY_IDX = [11, 12, 13, 14, 15, 16]

# 하체 감지 최소 confidence
LOWER_BODY_CONF_THRESHOLD = 0.3

# 빈 반환값 상수
_EMPTY = ([], None, np.zeros((0, 17, 2)), np.zeros((0, 17)))


class KeypointExtractor:
    """
    YOLO11 Pose로 사람 감지 + 17관절 동시 추출

    전신 필터 적용:
        하체 관절(엉덩이~발목) confidence 평균이
        LOWER_BODY_CONF_THRESHOLD 미만이면 스킵
        → CCTV 사각지대 / 부분 가시성 오탐 방지

    사용 예시:
        extractor = KeypointExtractor()
        results, raw_det, kp_data, kp_conf = extractor.extract(frame)
    """

    def __init__(self):
        self.yolo = None
        self._init_model()

    def _init_model(self):
        """YOLO11 Pose 모델 로드"""
        try:
            from ultralytics import YOLO
            self.yolo = YOLO("yolo11n-pose.pt")
            logger.info("[Extractor] YOLO11 Pose 로드 완료")
        except Exception as e:
            logger.error(f"[Extractor] YOLO11 Pose 로드 실패: {e}")
            self.yolo = None

    def extract(self, frame: np.ndarray) -> tuple:
        """
        프레임에서 전신이 보이는 사람만 Keypoint 추출

        입력:
            frame: BGR 이미지 (np.ndarray)

        반환:
            (results, detections, keypoints_data, keypoints_conf)

            results: list of dict [
                {
                    "track_id": int,
                    "keypoints": np.ndarray (34,),
                    "bbox":      (x1, y1, x2, y2)
                }, ...
            ]
            detections:     YOLO raw 결과 객체 (디버그용)
            keypoints_data: np.ndarray (N, 17, 2) (디버그용)
            keypoints_conf: np.ndarray (N, 17)    (디버그용)
        """
        if self.yolo is None:
            return _EMPTY

        # 해상도 조절
        frame = cv2.resize(frame, AI_RESOLUTION)

        results = []
        try:
            detections = self.yolo(frame, verbose=False)[0]

            if detections.keypoints is None:
                return _EMPTY

            keypoints_data = detections.keypoints.xy.cpu().numpy()    # (N, 17, 2)
            keypoints_conf = detections.keypoints.conf.cpu().numpy()  # (N, 17)
            boxes          = detections.boxes

            for i, kp_17 in enumerate(keypoints_data):

                # ── 전신 필터: 하체 confidence 체크 ─────────────
                lower_conf = keypoints_conf[i][LOWER_BODY_IDX].mean()
                if lower_conf < LOWER_BODY_CONF_THRESHOLD:
                    logger.debug(
                        f"[Extractor] 전신 미감지 → 스킵 "
                        f"(lower_conf={lower_conf:.2f} < {LOWER_BODY_CONF_THRESHOLD})"
                    )
                    continue

                # ── Keypoint 추출 (픽셀 좌표 그대로) ────────────
                # Scaler가 픽셀 좌표 기준으로 학습됨 → 정규화 안 함
                kp = kp_17.flatten().astype(np.float32)  # (34,)

                # bbox 추출
                bbox = (0, 0, 0, 0)
                if boxes is not None and i < len(boxes):
                    bbox = tuple(map(int, boxes.xyxy[i].tolist()))

                results.append({
                    "track_id": i,
                    "keypoints": kp,
                    "bbox":      bbox
                })

        except Exception as e:
            logger.error(f"[Extractor] 추출 오류: {e}")
            return _EMPTY

        return results, detections, keypoints_data, keypoints_conf