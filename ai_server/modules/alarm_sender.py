"""
modules/alarm_sender.py
=======================
Node 3 (메인 서버) HTTP POST 알람 전송 모듈

전송 조건:
    FALL 판정 시에만 전송
    UNCERTAIN은 전송 안 함 (AI 서버 자체 저장)

중복 알람 방지:
    동일 camera_id에서 ALARM_COOLDOWN_SEC 이내 재전송 차단

재시도:
    전송 실패 시 최대 NODE3_RETRY_COUNT 회 재시도
"""

import time
import json
import logging
import requests
from datetime import datetime, timezone
from config import (
    NODE3_FALL_ENDPOINT,
    NODE3_RETRY_COUNT,
    ALARM_COOLDOWN_SEC
)
from datetime import datetime, timezone

logger = logging.getLogger(__name__)


class AlarmSender:
    """
    낙상 감지 알람 전송

    사용 예시:
        sender = AlarmSender()
s       ender.send(camera_id=2, confidence=0.94, timestamp=1713340120000)
    """

    def __init__(self):
        # 쿨다운 관리: {camera_id: last_sent_timestamp}
        self._last_sent: dict[int, float] = {}

    def send(self, camera_id: int, confidence: float, timestamp: int) -> bool:

        """
        Node 3으로 낙상 알람 전송

        반환:
            True  → 전송 성공
            False → 쿨다운 중 or 전송 실패
        """
        # 쿨다운 체크
        if self._is_cooldown(camera_id):
            remaining = self._cooldown_remaining(camera_id)
            logger.debug(
                f"[Alarm] 쿨다운 중 (cam={camera_id}, "
                f"잔여 {remaining:.1f}초) → 전송 스킵"
            )
            return False

        payload = {
            "event": "fall_detected",
            "camera_id": camera_id,
            "timestamp": datetime.fromtimestamp(
                timestamp / 1000,
                tz=timezone.utc
            ).isoformat(),  # int → str 변환
            "confidence": round(confidence, 4),
        }

        success = self._post_with_retry(payload)

        if success:
            self._last_sent[camera_id] = time.time()
            logger.info(
                f"[Alarm] 전송 성공 → Node 3 "
                f"(cam={camera_id}, conf={confidence:.3f})"
            )
        else:
            logger.error(
                f"[Alarm] 전송 실패 "
                f"(cam={camera_id}, conf={confidence:.3f})"
            )

        return success

    # 내부 메서드

    def _post_with_retry(self, payload: dict) -> bool:
        """재시도 포함 HTTP POST 전송"""
        for attempt in range(1, NODE3_RETRY_COUNT + 1):
            try:
                response = requests.post(
                    NODE3_FALL_ENDPOINT,
                    json=payload,
                    timeout=3.0
                )
                if response.status_code == 200:
                    return True
                else:
                    logger.warning(
                        f"[Alarm] HTTP {response.status_code} "
                        f"(시도 {attempt}/{NODE3_RETRY_COUNT})"
                    )
            except requests.exceptions.ConnectionError:
                logger.warning(
                    f"[Alarm] Node 3 연결 실패 "
                    f"(시도 {attempt}/{NODE3_RETRY_COUNT})"
                )
            except requests.exceptions.Timeout:
                logger.warning(
                    f"[Alarm] 타임아웃 "
                    f"(시도 {attempt}/{NODE3_RETRY_COUNT})"
                )
            except Exception as e:
                logger.error(f"[Alarm] 전송 오류: {e}")

            if attempt < NODE3_RETRY_COUNT:
                time.sleep(0.5 * attempt)  # 점진적 대기

        return False

    def _is_cooldown(self, camera_id: int) -> bool:
        """쿨다운 중인지 확인"""
        if camera_id not in self._last_sent:
            return False
        return (time.time() - self._last_sent[camera_id]) < ALARM_COOLDOWN_SEC

    def _cooldown_remaining(self, camera_id: int) -> float:
        """쿨다운 잔여 시간(초) 반환"""
        if camera_id not in self._last_sent:
            return 0.0
        elapsed = time.time() - self._last_sent[camera_id]
        return max(0.0, ALARM_COOLDOWN_SEC - elapsed)
