"""
modules/keypoint_extractor.py
=============================
YOLO11 Pose 기반 Keypoint 추출 모듈

추출 방식: YOLO11 Pose 단일 forward pass
관절 수:   COCO 17관절 × (x, y) = 34피처
"""

import cv2
import numpy as np
import logging
from config import N_FEATURES, AI_RESOLUTION

logger = logging.getLogger(__name__)

# COCO 17관절 순서 (모델 keypoint_order와 동일)
COCO_KEYPOINTS = [
    'Nose', 'Left Eye', 'Right Eye', 'Left Ear', 'Right Ear',
    'Left Shoulder', 'Right Shoulder', 'Left Elbow', 'Right Elbow',
    'Left Wrist', 'Right Wrist', 'Left Hip', 'Right Hip',
    'Left Knee', 'Right Knee', 'Left Ankle', 'Right Ankle'
]


class KeypointExtractor:
    """
    YOLO11 Pose로 사람 감지 + 17관절 동시 추출

    사용 예시:
        extractor = KeypointExtractor()
        results = extractor.extract(frame)
        # results: [{"track_id": 1, "keypoints": np.array(34,), "bbox": (x1,y1,x2,y2)}]
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

    def extract(self, frame: np.ndarray) -> list:
        """
        프레임에서 사람별 Keypoint 추출

        입력:
            frame: BGR 이미지 (np.ndarray)

        반환:
            list of dict: [
                {
                    "track_id": int,
                    "keypoints": np.ndarray (34,),  ← 17관절 × x,y 정규화
                    "bbox": (x1, y1, x2, y2)
                },
                ...
            ]
            사람 없거나 모델 없으면 빈 리스트 반환
        """
        if self.yolo is None:
            return []

        # 해상도 조절
        frame = cv2.resize(frame, AI_RESOLUTION)
        h, w = frame.shape[:2]

        results = []
        try:
            detections = self.yolo(frame, verbose=False)[0]

            if detections.keypoints is None:
                return []

            keypoints_data = detections.keypoints.xy.cpu().numpy()  # (N, 17, 2)
            boxes          = detections.boxes

            for i, kp_17 in enumerate(keypoints_data):
                # 17관절 × 2축 = 34피처
                kp = kp_17.flatten().astype(np.float32)  # (34,)

                # 좌표 정규화 (0~1)
                kp[0::2] /= w   # X 정규화
                kp[1::2] /= h   # Y 정규화

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

        return results