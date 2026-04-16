"""
modules/keypoint_extractor.py
=============================
모델 버전별 Keypoint 추출 모듈

모델 1: YOLO → BBox 크롭 → MediaPipe BlazePose (33관절, 99피처)
모델 2: YOLO11 Pose (17관절, 34피처) - 단일 forward pass
"""

import cv2
import numpy as np
import logging
from config import MODEL_VERSION, N_FEATURES, AI_RESOLUTION

logger = logging.getLogger(__name__)

# ── BlazePose 33관절 → COCO 17관절 매핑 인덱스 ───────────────
# UP-Fall 컬럼 순서: Joint1_X(0), Joint1_Y(1), Joint1_Z(2), Joint2_X(3)...
# 추출할 BlazePose 관절 번호 (0-indexed): 17관절
BLAZEPOSE_TO_COCO_IDX = [0, 2, 5, 7, 8, 11, 12, 13, 14, 15, 16, 23, 24, 25, 26, 27, 28]

def _get_coco_feature_indices():
    """17관절의 X, Y 피처 인덱스 반환 (Z 제외)"""
    indices = []
    for joint_idx in BLAZEPOSE_TO_COCO_IDX:
        base = joint_idx * 3
        indices.append(base)      # X
        indices.append(base + 1)  # Y
    return indices  # 34개

COCO_FEATURE_INDICES = _get_coco_feature_indices()


class KeypointExtractor:
    """
    모델 버전에 따라 자동으로 올바른 추출기 사용
    """

    def __init__(self):
        self.model_version = MODEL_VERSION
        self._init_models()

    def _init_models(self):
        """모델 버전에 맞는 도구 초기화"""
        if self.model_version == 1:
            self._init_model1()
        elif self.model_version == 2:
            self._init_model2()

    def _init_model1(self):
        """모델 1: YOLO + MediaPipe"""
        try:
            from ultralytics import YOLO
            self.yolo = YOLO("yolov8n.pt")
            logger.info("[Extractor] YOLO 로드 완료")
        except Exception as e:
            logger.error(f"[Extractor] YOLO 로드 실패: {e}")
            self.yolo = None

        try:
            import mediapipe as mp
            from mediapipe.tasks.python.vision import PoseLandmarker
            from mediapipe.tasks.python.vision.pose_landmarker import PoseLandmarkerOptions
            from mediapipe.tasks.python import BaseOptions
            from mediapipe.tasks.python.vision.core.vision_task_running_mode import \
                VisionTaskRunningMode as VisionRunningMode

            options = PoseLandmarkerOptions(
                base_options=BaseOptions(model_asset_path="pose_landmarker_lite.task"),
                running_mode=VisionRunningMode.IMAGE,
                min_pose_detection_confidence=0.5,
                min_tracking_confidence=0.5
            )
            self.mp_pose = PoseLandmarker.create_from_options(options)
            self.mp = mp
            logger.info("[Extractor] MediaPipe 로드 완료")
        except Exception as e:
            logger.error(f"[Extractor] MediaPipe 로드 실패: {e}")
            self.mp_pose = None
            self.mp = None

    def _init_model2(self):
        """모델 2: YOLO11 Pose"""
        try:
            from ultralytics import YOLO
            self.yolo11_pose = YOLO("yolo11n-pose.pt")
            logger.info("[Extractor] YOLO11 Pose 로드 완료")
        except Exception as e:
            logger.error(f"[Extractor] YOLO11 Pose 로드 실패: {e}")
            self.yolo11_pose = None

    # ── 공개 메서드 ──────────────────────────────────────────

    def extract(self, frame: np.ndarray) -> list:
        """
        프레임에서 Keypoint 추출

        반환:
            list of dict: [
                {
                    "track_id": int,
                    "keypoints": np.ndarray (N_FEATURES,),
                    "bbox": (x1, y1, x2, y2)
                },
                ...
            ]
            사람 없으면 빈 리스트 반환
        """
        # 해상도 조절
        frame = cv2.resize(frame, AI_RESOLUTION)

        if self.model_version == 1:
            return self._extract_v1(frame)
        elif self.model_version == 2:
            return self._extract_v2(frame)
        return []

    # ── 모델 1: YOLO + MediaPipe ─────────────────────────────

    def _extract_v1(self, frame: np.ndarray) -> list:
        """YOLO로 사람 감지 → 크롭 → MediaPipe 33관절 추출"""
        if self.yolo is None or self.mp_pose is None:
            return []

        results = []
        try:
            detections = self.yolo(frame, classes=[0], verbose=False)[0]
            boxes = detections.boxes

            if boxes is None or len(boxes) == 0:
                return []

            for i, box in enumerate(boxes):
                x1, y1, x2, y2 = map(int, box.xyxy[0].tolist())
                if (x2 - x1) < 30 or (y2 - y1) < 30:
                    continue

                cropped = frame[y1:y2, x1:x2]

                # ── 여기서부터 신버전으로 변경 ──────────────────
                rgb = cv2.cvtColor(cropped, cv2.COLOR_BGR2RGB)
                mp_image = self.mp.Image(
                    image_format=self.mp.ImageFormat.SRGB,
                    data=rgb
                )
                pose_result = self.mp_pose.detect(mp_image)

                if not pose_result.pose_landmarks:
                    continue

                # 33관절 × 3축 = 99 피처 추출
                kp = []
                for lm in pose_result.pose_landmarks[0]:  # [0] = 첫 번째 사람
                    kp.extend([lm.x, lm.y, lm.z])
                kp = np.array(kp, dtype=np.float32)  # (99,)
                # ── 여기까지 ─────────────────────────────────────

                results.append({
                    "track_id": i,
                    "keypoints": kp,
                    "bbox": (x1, y1, x2, y2)
                })

        except Exception as e:
            logger.error(f"[Extractor v1] 추출 오류: {e}")

        return results

    # ── 모델 2: YOLO11 Pose ──────────────────────────────────

    def _extract_v2(self, frame: np.ndarray) -> list:
        """YOLO11 Pose로 사람 감지 + 17관절 동시 추출"""
        if self.yolo11_pose is None:
            return []

        results = []
        try:
            detections = self.yolo11_pose(frame, verbose=False)[0]

            if detections.keypoints is None:
                return []

            keypoints_data = detections.keypoints.xy.cpu().numpy()  # (N, 17, 2)
            boxes = detections.boxes

            for i, kp_17 in enumerate(keypoints_data):
                # 17관절 × 2축 = 34 피처
                kp = kp_17.flatten().astype(np.float32)  # (34,)

                # 좌표 정규화 (0~1)
                h, w = frame.shape[:2]
                kp[0::2] /= w   # X 정규화
                kp[1::2] /= h   # Y 정규화

                bbox = (0, 0, 0, 0)
                if boxes is not None and i < len(boxes):
                    bbox = tuple(map(int, boxes.xyxy[i].tolist()))

                results.append({
                    "track_id": i,
                    "keypoints": kp,
                    "bbox": bbox
                })

        except Exception as e:
            logger.error(f"[Extractor v2] 추출 오류: {e}")

        return results
