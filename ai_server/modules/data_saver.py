"""
modules/data_saver.py
=====================
자동 저장 모듈 (재학습 데이터 수집)

저장 조건:
    FALL      -> CSV(Keypoint) + 영상 저장 + SQLite 기록
    UNCERTAIN -> CSV(Keypoint) + 영상 저장 + SQLite 기록
    NON_FALL  -> 저장 안 함 (폐기)

SQLite:
    파일: data/falsight_ai.db
    테이블: retrain_logs
    용도: 재학습 데이터 조회/필터링/관리
    특징: Python 내장, 별도 설치 불필요
"""

import os
import csv
import cv2
import sqlite3
import numpy as np
import logging
from datetime import datetime, timezone
from collections import deque
from config import (
    SAVE_DIR_FALL, SAVE_DIR_UNCERTAIN, SAVE_DIR_NON_FALL,
    SAVE_DIR_RAW, N_FEATURES, FRAME_WINDOW,
    SQLITE_DB_PATH
)
from modules.fall_detector import RESULT_FALL, RESULT_UNCERTAIN

logger = logging.getLogger(__name__)

VIDEO_BUFFER_MAXLEN = 300


class DataSaver:
    """
    FALL / UNCERTAIN 판정 시 CSV + 영상 자동 저장 + SQLite 기록
    """

    def __init__(self):
        self._ensure_dirs()
        self.video_buffers: dict[int, deque] = {}
        self._init_db()

    def _ensure_dirs(self):
        """저장 폴더 없으면 자동 생성"""
        for d in [SAVE_DIR_FALL, SAVE_DIR_UNCERTAIN, SAVE_DIR_NON_FALL, SAVE_DIR_RAW]:
            os.makedirs(d, exist_ok=True)
        # DB 파일 저장 폴더도 생성
        os.makedirs(os.path.dirname(SQLITE_DB_PATH), exist_ok=True)

    def _init_db(self):
        """SQLite DB 및 테이블 초기화"""
        try:
            conn = sqlite3.connect(SQLITE_DB_PATH)
            conn.execute("""
                CREATE TABLE IF NOT EXISTS retrain_logs (
                    id          INTEGER PRIMARY KEY AUTOINCREMENT,
                    timestamp   TEXT    NOT NULL,
                    camera_id   INTEGER NOT NULL,
                    track_id    INTEGER NOT NULL,
                    label       TEXT    NOT NULL,
                    confidence  REAL    NOT NULL,
                    csv_path    TEXT,
                    video_path  TEXT,
                    retrained   INTEGER DEFAULT 0
                )
            """)
            conn.commit()
            conn.close()
            logger.info(f"[Saver] SQLite 초기화 완료: {SQLITE_DB_PATH}")
        except Exception as e:
            logger.error(f"[Saver] SQLite 초기화 실패: {e}")

    def add_frame(self, camera_id: int, frame: np.ndarray):
        """매 프레임 호출 -> 영상 순환 버퍼에 누적"""
        if camera_id not in self.video_buffers:
            self.video_buffers[camera_id] = deque(maxlen=VIDEO_BUFFER_MAXLEN)
        self.video_buffers[camera_id].append(frame.copy())

    def save(
        self,
        label: str,
        camera_id: int,
        track_id: int,
        buffer: np.ndarray,
        confidence: float
    ):
        """
        FALL 또는 UNCERTAIN 판정 시 데이터 저장

        입력:
            label:      "FALL" | "UNCERTAIN"
            camera_id:  카메라 번호
            track_id:   ByteTrack ID (재학습 데이터 관리용)
            buffer:     shape (FRAME_WINDOW, N_FEATURES)
            confidence: CNN 출력 확률
        """
        if label not in [RESULT_FALL, RESULT_UNCERTAIN]:
            return  # NON_FALL은 저장 안 함

        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
        base_name = f"{timestamp}_cam{camera_id}_track{track_id}_conf{confidence:.2f}"
        save_dir  = SAVE_DIR_FALL if label == RESULT_FALL else SAVE_DIR_UNCERTAIN

        # ① CSV 저장
        csv_path = self._save_csv(buffer, base_name, save_dir, label, confidence)

        # ② 영상 저장
        video_path = self._save_video(camera_id, base_name, save_dir)

        # ③ SQLite 기록
        self._save_db(
            label=label,
            camera_id=camera_id,
            track_id=track_id,
            confidence=confidence,
            csv_path=csv_path,
            video_path=video_path
        )

        logger.info(
            f"[Saver] {label} 저장 완료: {base_name} "
            f"(cam={camera_id}, track={track_id}, conf={confidence:.3f})"
        )

    # 내부 메서드

    def _save_csv(
        self,
        buffer: np.ndarray,
        base_name: str,
        save_dir: str,
        label: str,
        confidence: float
    ) -> str:
        """Keypoint 시퀀스를 CSV로 저장, 경로 반환"""
        csv_path = os.path.join(save_dir, f"{base_name}.csv")
        try:
            with open(csv_path, "w", newline="") as f:
                writer = csv.writer(f)
                header = [f"f{i}" for i in range(N_FEATURES)] + ["LABEL", "CONFIDENCE"]
                writer.writerow(header)
                for kp in buffer:
                    row = kp.tolist() + [
                        1 if label == RESULT_FALL else 0,
                        round(confidence, 4)
                    ]
                    writer.writerow(row)
            logger.debug(f"[Saver] CSV 저장: {csv_path}")
        except Exception as e:
            logger.error(f"[Saver] CSV 저장 실패: {e}")
            csv_path = ""
        return csv_path

    def _save_video(self, camera_id: int, base_name: str, save_dir: str) -> str:
        """영상 버퍼를 MP4로 저장, 경로 반환"""
        video_path = ""
        if camera_id not in self.video_buffers:
            return video_path
        frames = list(self.video_buffers[camera_id])
        if not frames:
            return video_path

        video_path = os.path.join(save_dir, f"{base_name}.mp4")
        try:
            h, w = frames[0].shape[:2]
            fourcc = cv2.VideoWriter_fourcc(*"mp4v")
            writer = cv2.VideoWriter(video_path, fourcc, 30.0, (w, h))
            for frame in frames:
                writer.write(frame)
            writer.release()
            logger.debug(f"[Saver] 영상 저장: {video_path} ({len(frames)}프레임)")
        except Exception as e:
            logger.error(f"[Saver] 영상 저장 실패: {e}")
            video_path = ""
        return video_path

    def _save_db(
        self,
        label: str,
        camera_id: int,
        track_id: int,
        confidence: float,
        csv_path: str,
        video_path: str
    ):
        """SQLite에 메타데이터 기록"""
        try:
            conn = sqlite3.connect(SQLITE_DB_PATH)
            conn.execute("""
                INSERT INTO retrain_logs
                    (timestamp, camera_id, track_id, label,
                     confidence, csv_path, video_path, retrained)
                VALUES (?, ?, ?, ?, ?, ?, ?, 0)
            """, (
                datetime.now(timezone.utc).isoformat(),
                camera_id,
                track_id,
                label,
                round(confidence, 4),
                csv_path,
                video_path
            ))
            conn.commit()
            conn.close()
            logger.debug(f"[Saver] SQLite 기록 완료")
        except Exception as e:
            logger.error(f"[Saver] SQLite 저장 실패: {e}")