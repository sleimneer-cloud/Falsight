
import logging
import multiprocessing as mp


def alarm_process(result_queue: mp.Queue):
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
            track_id = result["track_id"]
            confidence = result["confidence"]
            timestamp = result["timestamp"]

            log.info(
                f"[Alarm] FALL 수신 → 전송 시도 "
                f"(cam={camera_id}, track={track_id}, conf={confidence:.3f})"
            )

            sender.send(
                camera_id=camera_id,
                confidence=confidence,
                timestamp=timestamp
            )

        except mp.queues.Empty:
            continue
        except Exception as e:
            log.error(f"[Alarm] 오류: {e}", exc_info=True)
            continue

    log.info("[Alarm] 알람 프로세스 종료")
