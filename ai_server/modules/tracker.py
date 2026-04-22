"""
modules/tracker.py
==================
ByteTrack 기반 객체 추적 모듈

역할:
    - 최초 YOLO 감지 시 track_id 부여
    - 이후 프레임은 추적기만으로 BBox 유지
    - 정지 상태에서도 track_id 유지 (낙상 후 누운 자세 포착)
    - 여러 명 동시 독립 추적
"""

import numpy as np
import logging
from collections import defaultdict

logger = logging.getLogger(__name__)


class SimpleTracker:
    """
    ByteTrack 라이브러리 미설치 환경을 위한 경량 대체 추적기
    ByteTrack 설치 시 ByteTracker 클래스로 교체

    동작 방식:
        IoU 기반 BBox 매칭으로 프레임 간 동일 객체 연결
        일정 프레임 동안 감지 안 되면 추적 종료
    """

    def __init__(self, max_lost=30):
        """
        max_lost: 이 프레임 수 동안 감지 안 되면 추적 종료
                  30프레임 = 1초 (30fps 기준)
                  낙상 후 정지 상태에서도 1초간 유지
        """
        self.max_lost  = max_lost
        self.tracks    = {}    # {track_id: {"bbox": ..., "lost": 0}}
        self.next_id   = 1

    def update(self, detections: list) -> list:
        """
        감지 결과로 추적 상태 갱신

        입력:
            detections: [{"bbox": (x1,y1,x2,y2), "keypoints": ..., ...}, ...]

        반환:
            tracks: [{"track_id": int, "bbox": ..., "keypoints": ...}, ...]
        """
        if not detections:
            # 감지 없음 → 모든 추적 대상 lost 카운트 증가
            self._increment_lost()
            return self._get_active_tracks_without_keypoints()

        det_bboxes = np.array([d["bbox"] for d in detections], dtype=np.float32)

        if not self.tracks:
            # 추적 중인 객체 없음 → 모두 새 track으로 등록
            return self._register_all(detections)

        # IoU 매칭
        track_ids   = list(self.tracks.keys())
        track_bboxes = np.array([self.tracks[tid]["bbox"] for tid in track_ids])
        iou_matrix  = self._compute_iou_matrix(track_bboxes, det_bboxes)

        matched_tracks = set()
        matched_dets   = set()
        result = []

        # IoU > 0.3이면 같은 객체로 매칭
        for t_idx, d_idx in zip(*np.where(iou_matrix > 0.3)):
            if t_idx in matched_tracks or d_idx in matched_dets:
                continue
            tid = track_ids[t_idx]
            self.tracks[tid]["bbox"] = detections[d_idx]["bbox"]
            self.tracks[tid]["lost"] = 0
            matched_tracks.add(t_idx)
            matched_dets.add(d_idx)
            result.append({
                "track_id":  tid,
                "bbox":      detections[d_idx]["bbox"],
                "keypoints": detections[d_idx]["keypoints"]
            })

        # 매칭 안 된 감지 → 새 track 등록
        for d_idx, det in enumerate(detections):
            if d_idx not in matched_dets:
                new_id = self.next_id
                self.next_id += 1
                self.tracks[new_id] = {"bbox": det["bbox"], "lost": 0}
                result.append({
                    "track_id":  new_id,
                    "bbox":      det["bbox"],
                    "keypoints": det["keypoints"]
                })

        # 매칭 안 된 track → lost 증가
        for t_idx, tid in enumerate(track_ids):
            if t_idx not in matched_tracks:
                self.tracks[tid]["lost"] += 1

        # max_lost 초과 track 제거
        self.tracks = {
            tid: info for tid, info in self.tracks.items()
            if info["lost"] <= self.max_lost
        }

        return result

    # 내부 메서드

    def _register_all(self, detections: list) -> list:
        result = []
        for det in detections:
            tid = self.next_id
            self.next_id += 1
            self.tracks[tid] = {"bbox": det["bbox"], "lost": 0}
            result.append({
                "track_id":  tid,
                "bbox":      det["bbox"],
                "keypoints": det["keypoints"]
            })
        return result

    def _increment_lost(self):
        for tid in list(self.tracks.keys()):
            self.tracks[tid]["lost"] += 1
            if self.tracks[tid]["lost"] > self.max_lost:
                del self.tracks[tid]

    def _get_active_tracks_without_keypoints(self):
        """감지 없을 때 활성 track 목록 반환 (keypoints 없음)"""
        return [
            {"track_id": tid, "bbox": info["bbox"], "keypoints": None}
            for tid, info in self.tracks.items()
        ]

    @staticmethod
    def _compute_iou_matrix(bboxes_a: np.ndarray, bboxes_b: np.ndarray) -> np.ndarray:
        """두 BBox 배열 간 IoU 행렬 계산"""
        iou = np.zeros((len(bboxes_a), len(bboxes_b)), dtype=np.float32)
        for i, a in enumerate(bboxes_a):
            for j, b in enumerate(bboxes_b):
                ix1 = max(a[0], b[0]); iy1 = max(a[1], b[1])
                ix2 = min(a[2], b[2]); iy2 = min(a[3], b[3])
                inter = max(0, ix2 - ix1) * max(0, iy2 - iy1)
                area_a = (a[2]-a[0]) * (a[3]-a[1])
                area_b = (b[2]-b[0]) * (b[3]-b[1])
                union = area_a + area_b - inter
                iou[i][j] = inter / union if union > 0 else 0
        return iou

    def reset(self):
        """카메라 재연결 시 버퍼 초기화"""
        self.tracks  = {}
        self.next_id = 1
        logger.info("[Tracker] 추적 초기화")
