"""
modules/alarm_process.py
========================
알람 프로세스

역할:
    Result Queue에서 FALL 결과 수신
    → AlarmSender로 Node 3에 HTTP POST 전송
    → 쿨다운 관리 (동일 카메라 중복 알람 방지)
"""

import logging
import multiprocessing as mp


def alarm_process(result_queue: mp.Queue):
    """
    알람 프로세스 진입점

    입력:
        result_queue: 워커 프로세스로부터 FALL 결과 수신
    """
    from modules.alarm_sender import AlarmSender

    logging.basicConfig(level=logging.INFO)
    log = logging.getLogger("AlarmProcess")
    log.info("[Alarm] 알람 프로세스 시작")

    sender = AlarmSender()

    while True:
        try:
            # Result Queue에서 FALL 결과 수신
            result = result_queue.get(timeout=1.0)

            if result is None:
                log.info("[Alarm] 종료 신호 수신")
                break

            camera_id  = result["camera_id"]
            confidence = result["confidence"]

            log.info(
                f"[Alarm] FALL 수신 → 전송 시도 "
                f"(cam={camera_id}, conf={confidence:.3f})"
            )

            sender.send(
                camera_id=camera_id,
                confidence=confidence
            )

        except mp.queues.Empty:
            continue
        except Exception as e:
            log.error(f"[Alarm] 오류: {e}", exc_info=True)
            continue

    log.info("[Alarm] 알람 프로세스 종료")
