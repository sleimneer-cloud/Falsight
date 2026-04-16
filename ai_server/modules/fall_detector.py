"""
modules/fall_detector.py
========================
CNN 추론 및 낙상 판정 모듈

판정 결과:
    FALL      : confidence >= FALL_THRESHOLD
    UNCERTAIN : UNCERTAIN_MIN <= confidence < UNCERTAIN_MAX
    NON_FALL  : confidence < UNCERTAIN_MIN
"""

import numpy as np
import logging
import time
from config import (
    MODEL_PATH, FRAME_WINDOW, N_FEATURES,
    FALL_THRESHOLD, UNCERTAIN_MIN, UNCERTAIN_MAX,
    INFERENCE_TIMEOUT_MS,
    USE_GPU
)

if USE_GPU:
    # GPU 환경 (RTX 3060)
    import tensorflow as tf
    physical_devices = tf.config.list_physical_devices('GPU')
    if physical_devices:
        tf.config.experimental.set_memory_growth(physical_devices[0], True)
else:
    # CPU 환경 (현재)
    import tensorflow as tf
    tf.config.set_visible_devices([], 'GPU')

logger = logging.getLogger(__name__)

# 판정 결과 상수
RESULT_FALL      = "FALL"
RESULT_UNCERTAIN = "UNCERTAIN"
RESULT_NON_FALL  = "NON_FALL"


class FallDetector:
    """
    학습된 CNN 모델 로드 및 낙상 판정

    사용 예시:
        detector = FallDetector()
        result = detector.predict(buffer_array)
        # result: {"label": "FALL", "confidence": 0.94}
    """

    def __init__(self):
        self.model = None
        self._load_model()

    def _load_model(self):
        """모델 파일 로드 (실패 시 서버 기동 중단)"""
        try:
            self.model = tf.keras.models.load_model(MODEL_PATH)
            logger.info(f"[Detector] 모델 로드 완료: {MODEL_PATH}")
            logger.info(f"[Detector] 입력 shape: (batch, {FRAME_WINDOW}, {N_FEATURES})")
        except Exception as e:
            if "File not found" in str(e) or "No such file" in str(e):
                logger.warning(
                    f"[Detector] 모델 파일 없음: {MODEL_PATH} "
                    f"→ 더미 모드로 실행 (테스트용)"
                )
                self.model = None
            else:
                logger.error(f"[Detector] 모델 로드 실패: {e}")
                raise RuntimeError(f"모델 로드 실패 — 서버 기동 중단: {e}")

    def predict(self, buffer: np.ndarray) -> dict | None:
        """
        100프레임 버퍼로 낙상 판정

        입력:
            buffer: shape (FRAME_WINDOW, N_FEATURES)

        반환:
            {
                "label":      "FALL" | "UNCERTAIN" | "NON_FALL",
                "confidence": float (0.0~1.0)
            }
            타임아웃 초과 시 None 반환
        """
        if buffer is None or buffer.shape != (FRAME_WINDOW, N_FEATURES):
            logger.warning(f"[Detector] 잘못된 버퍼 shape: {buffer.shape if buffer is not None else None}")
            return None

        start = time.time()

        try:
            if self.model is None:
                # 더미 모드: 랜덤 confidence (테스트용)
                confidence = float(np.random.uniform(0, 1))
                logger.debug("[Detector] 더미 모드 추론")
            else:
                # 실제 추론
                input_data = buffer[np.newaxis, ...]  # (1, 100, N)
                output = self.model.predict(input_data, verbose=0)
                confidence = float(output[0][0])

            # 타임아웃 체크
            elapsed_ms = (time.time() - start) * 1000
            if elapsed_ms > INFERENCE_TIMEOUT_MS:
                logger.warning(
                    f"[Detector] 타임아웃 초과: {elapsed_ms:.1f}ms > {INFERENCE_TIMEOUT_MS}ms "
                    f"→ 프레임 폐기"
                )
                return None

            # 판정
            label = self._classify(confidence)

            logger.debug(
                f"[Detector] 판정: {label} "
                f"(confidence={confidence:.3f}, {elapsed_ms:.1f}ms)"
            )

            return {
                "label":      label,
                "confidence": round(confidence, 4)
            }

        except Exception as e:
            logger.error(f"[Detector] 추론 오류: {e}")
            return None

    @staticmethod
    def _classify(confidence: float) -> str:
        """confidence 값으로 판정 결과 반환"""
        if confidence >= FALL_THRESHOLD:
            return RESULT_FALL
        elif confidence >= UNCERTAIN_MIN:
            return RESULT_UNCERTAIN
        else:
            return RESULT_NON_FALL