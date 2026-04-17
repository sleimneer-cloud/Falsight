"""
modules/fall_detector.py
========================
PyTorch LSTM 추론 및 낙상 판정 모듈

모델 스펙 (fallvision_best_model_v3.pt):
    input_size  : 34 (COCO 17관절 × x, y)
    hidden_size : 128
    num_layers  : 2
    dropout     : 0.3
    threshold   : 0.5 (모델 내장값 사용)

판정 결과:
    FALL      : confidence >= FALL_THRESHOLD
    UNCERTAIN : UNCERTAIN_MIN <= confidence < UNCERTAIN_MAX
    NON_FALL  : confidence < UNCERTAIN_MIN
"""

import numpy as np
import logging
import time
import torch
import torch.nn as nn

from config import (
    MODEL_PATH, FRAME_WINDOW, N_FEATURES,
    FALL_THRESHOLD, UNCERTAIN_MIN, UNCERTAIN_MAX,
    INFERENCE_TIMEOUT_MS, USE_GPU
)

logger = logging.getLogger(__name__)

# 판정 결과 상수
RESULT_FALL      = "FALL"
RESULT_UNCERTAIN = "UNCERTAIN"
RESULT_NON_FALL  = "NON_FALL"


# ── LSTM 모델 정의 ────────────────────────────────────────────
class FallLSTM(nn.Module):
    """
    학습 시 사용한 LSTM 구조와 동일하게 정의
    (state_dict 로드를 위해 구조 일치 필수)
    """
    def __init__(self, input_size, hidden_size, num_layers, dropout):
        super().__init__()
        self.lstm = nn.LSTM(
            input_size  = input_size,
            hidden_size = hidden_size,
            num_layers  = num_layers,
            batch_first = True,
            dropout     = dropout if num_layers > 1 else 0.0
        )
        self.fc = nn.Linear(hidden_size, 1)
        self.sigmoid = nn.Sigmoid()

    def forward(self, x):
        # x: (batch, seq_len, input_size)
        out, _ = self.lstm(x)
        out = self.fc(out[:, -1, :])   # 마지막 타임스텝
        return self.sigmoid(out)


# ── FallDetector ─────────────────────────────────────────────
class FallDetector:
    """
    PyTorch LSTM 모델 로드 및 낙상 판정

    사용 예시:
        detector = FallDetector()
        result = detector.predict(buffer_array)
        # result: {"label": "FALL", "confidence": 0.94}
    """

    def __init__(self):
        self.model     = None
        self.device    = torch.device("cuda" if USE_GPU and torch.cuda.is_available() else "cpu")
        self.threshold = FALL_THRESHOLD   # config 기본값, 모델 내장값으로 덮어씀
        self._load_model()

    def _load_model(self):
        """PyTorch LSTM 모델 로드"""
        try:
            checkpoint = torch.load(MODEL_PATH, map_location=self.device)

            # 모델 구조 파라미터 추출
            input_size  = checkpoint["input_size"]
            hidden_size = checkpoint["hidden_size"]
            num_layers  = checkpoint["num_layers"]
            dropout     = checkpoint["dropout"]

            # 모델 내장 threshold 사용 (config 값 덮어씀)
            self.threshold = FALL_THRESHOLD

            # 모델 생성 및 가중치 로드
            self.model = FallLSTM(input_size, hidden_size, num_layers, dropout)
            self.model.load_state_dict(checkpoint["model_state_dict"])
            self.model.to(self.device)
            self.model.eval()

            logger.info(f"[Detector] 모델 로드 완료: {MODEL_PATH}")
            logger.info(f"[Detector] 디바이스: {self.device}")
            logger.info(f"[Detector] 입력 shape: (batch, {FRAME_WINDOW}, {input_size})")
            logger.info(f"[Detector] threshold: {self.threshold}")

        except FileNotFoundError:
            logger.warning(
                f"[Detector] 모델 파일 없음: {MODEL_PATH} "
                f"→ 더미 모드로 실행 (테스트용)"
            )
            self.model = None

        except Exception as e:
            logger.error(f"[Detector] 모델 로드 실패: {e}")
            raise RuntimeError(f"모델 로드 실패 — 서버 기동 중단: {e}")

    def predict(self, buffer: np.ndarray) -> dict | None:
        """
        100프레임 버퍼로 낙상 판정

        입력:
            buffer: shape (FRAME_WINDOW, N_FEATURES)
                    = (100, 34)

        반환:
            {
                "label":      "FALL" | "UNCERTAIN" | "NON_FALL",
                "confidence": float (0.0~1.0)
            }
            타임아웃 초과 또는 오류 시 None 반환
        """
        if buffer is None or buffer.shape != (FRAME_WINDOW, N_FEATURES):
            logger.warning(
                f"[Detector] 잘못된 버퍼 shape: "
                f"{buffer.shape if buffer is not None else None}"
            )
            return None

        start = time.time()

        try:
            if self.model is None:
                # 더미 모드 (모델 파일 없을 때)
                confidence = float(np.random.uniform(0, 1))
                logger.debug("[Detector] 더미 모드 추론")

            else:
                # PyTorch LSTM 추론
                tensor = torch.tensor(
                    buffer[np.newaxis, ...],   # (1, 100, 34)
                    dtype=torch.float32
                ).to(self.device)

                with torch.no_grad():
                    output = self.model(tensor)   # (1, 1)
                    confidence = float(output[0][0].cpu())

            # 타임아웃 체크
            elapsed_ms = (time.time() - start) * 1000
            if elapsed_ms > INFERENCE_TIMEOUT_MS:
                logger.warning(
                    f"[Detector] 타임아웃 초과: {elapsed_ms:.1f}ms "
                    f"> {INFERENCE_TIMEOUT_MS}ms → 프레임 폐기"
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

    def _classify(self, confidence: float) -> str:
        """confidence 값으로 판정 결과 반환"""
        if confidence >= self.threshold:
            return RESULT_FALL
        elif confidence >= UNCERTAIN_MIN:
            return RESULT_UNCERTAIN
        else:
            return RESULT_NON_FALL