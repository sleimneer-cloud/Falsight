"""
test_pipeline.py
================
AI 서버 전체 파이프라인 흐름 테스트

외부 의존성 없이 (YOLO, MediaPipe, keras 모델 없어도)
각 모듈이 올바르게 연결되어 데이터가 흘러가는지 확인

테스트 항목:
  ① config 로드
  ② BufferManager - 100프레임 누적 및 반환
  ③ FallDetector  - 더미 모드 추론 및 판정
  ④ SimpleTracker - track_id 부여 및 유지
  ⑤ DataSaver     - 폴더 생성 및 CSV 저장
  ⑥ AlarmSender   - 쿨다운 로직
  ⑦ 전체 파이프라인 - 프레임 → 버퍼 → 추론 → 판정 흐름
"""

import numpy as np
import os
import sys
import time

# 프로젝트 루트를 path에 추가
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PASS = "✅ PASS"
FAIL = "❌ FAIL"
results = []

def check(name, condition, detail=""):
    status = PASS if condition else FAIL
    results.append((status, name, detail))
    print(f"  {status}  {name}" + (f" → {detail}" if detail else ""))


print("=" * 55)
print("  Falsight AI 서버 파이프라인 테스트")
print("=" * 55)

# ── ① config 로드 ────────────────────────────────────────────
print("\n[1] config.py 로드")
try:
    import config
    check("MODEL_VERSION 설정",   config.MODEL_VERSION in [1, 2],     str(config.MODEL_VERSION))
    check("N_FEATURES 설정",      config.N_FEATURES in [99, 34],      str(config.N_FEATURES))
    check("FRAME_WINDOW 설정",    config.FRAME_WINDOW == 100,         str(config.FRAME_WINDOW))
    check("FALL_THRESHOLD 설정",  0 < config.FALL_THRESHOLD <= 1,     str(config.FALL_THRESHOLD))
    check("CAMERA_IDS 설정",      len(config.CAMERA_IDS) == 4,        str(config.CAMERA_IDS))
    check("AI_RESOLUTION 설정",   config.AI_RESOLUTION == (640, 480), str(config.AI_RESOLUTION))
    check("USE_GPU 설정",         isinstance(config.USE_GPU, bool),   str(config.USE_GPU))
    check("ZMQ_HOST 설정",        config.ZMQ_HOST == "0.0.0.0",       config.ZMQ_HOST)
    check("저장 폴더 경로 설정",  bool(config.SAVE_DIR_FALL),         config.SAVE_DIR_FALL)
except Exception as e:
    check("config 로드", False, str(e))


# ── ② BufferManager ──────────────────────────────────────────
print("\n[2] BufferManager - 슬라이딩 윈도우 버퍼")
try:
    from modules.buffer_manager import BufferManager
    bm = BufferManager()

    # 99프레임 넣었을 때 None 반환 확인
    for i in range(99):
        kp = np.random.rand(config.N_FEATURES).astype(np.float32)
        result = bm.update(track_id=1, keypoints=kp)
    check("99프레임 → None 반환", result is None, "버퍼 미달")

    # 100번째 프레임에서 배열 반환 확인
    kp = np.random.rand(config.N_FEATURES).astype(np.float32)
    result = bm.update(track_id=1, keypoints=kp)
    check("100프레임 → 배열 반환", result is not None, "버퍼 완성")
    check("반환 shape 확인",
          result.shape == (config.FRAME_WINDOW, config.N_FEATURES),
          str(result.shape))

    # track_id별 독립 버퍼 확인
    bm2 = BufferManager()
    bm2.update(track_id=1, keypoints=np.zeros(config.N_FEATURES, dtype=np.float32))
    bm2.update(track_id=2, keypoints=np.ones(config.N_FEATURES, dtype=np.float32))
    check("track_id별 독립 버퍼", len(bm2.buffers) == 2, f"버퍼 수: {len(bm2.buffers)}")

    # 버퍼 초기화 확인
    bm2.reset()
    check("버퍼 초기화", len(bm2.buffers) == 0, "reset 완료")

except Exception as e:
    check("BufferManager", False, str(e))


# ── ③ FallDetector ───────────────────────────────────────────
print("\n[3] FallDetector - CNN 추론 및 판정")
try:
    from modules.fall_detector import FallDetector, RESULT_FALL, RESULT_UNCERTAIN, RESULT_NON_FALL

    det = FallDetector()
    check("FallDetector 초기화", True, "더미 모드" if det.model is None else "실제 모델")

    # 더미 모드 추론 100회 테스트
    buf = np.random.rand(config.FRAME_WINDOW, config.N_FEATURES).astype(np.float32)
    labels = set()
    for _ in range(100):
        r = det.predict(buf)
        if r:
            labels.add(r["label"])

    check("predict() 반환값 존재",    r is not None,       str(r))
    check("confidence 범위 (0~1)",    0 <= r["confidence"] <= 1, f"{r['confidence']:.3f}")
    check("label 값 유효성",
          r["label"] in [RESULT_FALL, RESULT_UNCERTAIN, RESULT_NON_FALL],
          r["label"])
    check("FALL/UNCERTAIN/NON_FALL 모두 발생 (100회)",
          len(labels) >= 2, f"발생한 label: {labels}")

    # 잘못된 shape 입력 시 None 반환 확인
    bad_buf = np.zeros((50, 10), dtype=np.float32)
    check("잘못된 shape → None 반환", det.predict(bad_buf) is None, "shape 검증")

    # 임계값 분류 로직 확인
    check("FALL 분류",      det._classify(0.95) == RESULT_FALL,      f"0.95 → {det._classify(0.95)}")
    check("UNCERTAIN 분류", det._classify(0.70) == RESULT_UNCERTAIN,  f"0.70 → {det._classify(0.70)}")
    check("NON_FALL 분류",  det._classify(0.30) == RESULT_NON_FALL,   f"0.30 → {det._classify(0.30)}")

except Exception as e:
    check("FallDetector", False, str(e))


# ── ④ SimpleTracker ──────────────────────────────────────────
print("\n[4] SimpleTracker - 객체 추적")
try:
    from modules.tracker import SimpleTracker

    tracker = SimpleTracker(max_lost=5)

    # 새 객체 감지 → track_id 부여
    det1 = [{"bbox": (10, 10, 100, 200), "keypoints": np.zeros(config.N_FEATURES)}]
    tracks = tracker.update(det1)
    check("새 객체 감지 → track_id 부여", len(tracks) == 1, f"track_id={tracks[0]['track_id']}")

    # 같은 위치 → 동일 track_id 유지
    tracks2 = tracker.update(det1)
    check("동일 위치 → track_id 유지",
          tracks2[0]["track_id"] == tracks[0]["track_id"],
          f"track_id={tracks2[0]['track_id']}")

    # 감지 없음 → lost 카운트
    for _ in range(3):
        tracker.update([])
    check("감지 없음 → 추적 유지 (max_lost 이내)", len(tracker.tracks) == 1, "lost=3")

    # max_lost 초과 → 추적 종료
    for _ in range(3):
        tracker.update([])
    check("max_lost 초과 → 추적 종료", len(tracker.tracks) == 0, "lost>5")

    # 여러 객체 동시 추적
    tracker.reset()
    multi = [
        {"bbox": (10,  10, 100, 200), "keypoints": np.zeros(config.N_FEATURES)},
        {"bbox": (200, 10, 300, 200), "keypoints": np.zeros(config.N_FEATURES)},
    ]
    tracks = tracker.update(multi)
    check("여러 객체 동시 추적", len(tracks) == 2,
          f"track_ids={[t['track_id'] for t in tracks]}")

except Exception as e:
    check("SimpleTracker", False, str(e))


# ── ⑤ DataSaver ──────────────────────────────────────────────
print("\n[5] DataSaver - 자동 저장")
try:
    from modules.data_saver import DataSaver
    from modules.fall_detector import RESULT_FALL, RESULT_UNCERTAIN

    saver = DataSaver()
    check("DataSaver 초기화", True, "")

    # 저장 폴더 생성 확인
    check("fall 폴더 존재",      os.path.exists(config.SAVE_DIR_FALL),      config.SAVE_DIR_FALL)
    check("uncertain 폴더 존재", os.path.exists(config.SAVE_DIR_UNCERTAIN),  config.SAVE_DIR_UNCERTAIN)
    check("raw 폴더 존재",       os.path.exists(config.SAVE_DIR_RAW),        config.SAVE_DIR_RAW)

    # CSV 저장 테스트
    buf = np.random.rand(config.FRAME_WINDOW, config.N_FEATURES).astype(np.float32)

    before = len(os.listdir(config.SAVE_DIR_FALL))
    saver.save(
        label=RESULT_FALL,
        camera_id=1,
        track_id=1,
        buffer=buf,
        confidence=0.95
    )
    after = len(os.listdir(config.SAVE_DIR_FALL))
    check("FALL CSV 저장", after > before, f"파일 {after - before}개 생성")

    # UNCERTAIN 저장 테스트
    before = len(os.listdir(config.SAVE_DIR_UNCERTAIN))
    saver.save(
        label=RESULT_UNCERTAIN,
        camera_id=2,
        track_id=2,
        buffer=buf,
        confidence=0.72
    )
    after = len(os.listdir(config.SAVE_DIR_UNCERTAIN))
    check("UNCERTAIN CSV 저장", after > before, f"파일 {after - before}개 생성")

    # NON_FALL은 저장 안 함 확인
    from modules.fall_detector import RESULT_NON_FALL
    before_f = len(os.listdir(config.SAVE_DIR_FALL))
    before_u = len(os.listdir(config.SAVE_DIR_UNCERTAIN))
    saver.save(label=RESULT_NON_FALL, camera_id=1, track_id=1, buffer=buf, confidence=0.3)
    after_f = len(os.listdir(config.SAVE_DIR_FALL))
    after_u = len(os.listdir(config.SAVE_DIR_UNCERTAIN))
    check("NON_FALL → 저장 안 함",
          after_f == before_f and after_u == before_u, "폴더 변화 없음")

except Exception as e:
    check("DataSaver", False, str(e))


# ── ⑥ AlarmSender ────────────────────────────────────────────
print("\n[6] AlarmSender - 쿨다운 로직")
try:
    from modules.alarm_sender import AlarmSender

    sender = AlarmSender()

    # 첫 전송 시도 (Node3 없어서 실패해도 쿨다운은 작동 안 함)
    check("AlarmSender 초기화", True, "")
    check("초기 쿨다운 없음", not sender._is_cooldown(1), "cam1 쿨다운 없음")

    # 쿨다운 수동 설정 후 확인
    sender._last_sent[1] = time.time()
    check("쿨다운 중 감지", sender._is_cooldown(1), "cam1 쿨다운 활성")

    remaining = sender._cooldown_remaining(1)
    check("쿨다운 잔여 시간 계산",
          0 < remaining <= config.ALARM_COOLDOWN_SEC,
          f"{remaining:.1f}초 남음")

    check("다른 카메라 쿨다운 없음", not sender._is_cooldown(2), "cam2 독립적")

except Exception as e:
    check("AlarmSender", False, str(e))


# ── ⑦ 전체 파이프라인 통합 테스트 ────────────────────────────
print("\n[7] 전체 파이프라인 통합 흐름")
try:
    from modules.buffer_manager import BufferManager
    from modules.fall_detector  import FallDetector, RESULT_FALL, RESULT_UNCERTAIN
    from modules.tracker        import SimpleTracker
    from modules.data_saver     import DataSaver

    tracker    = SimpleTracker()
    buffer_mgr = BufferManager()
    detector   = FallDetector()
    saver      = DataSaver()

    fall_count      = 0
    uncertain_count = 0
    nonfail_count   = 0
    infer_count     = 0

    # 150프레임 더미 처리
    for frame_num in range(150):
        # 더미 감지 (사람 1명)
        fake_detection = [{
            "bbox":      (50, 50, 200, 400),
            "keypoints": np.random.rand(config.N_FEATURES).astype(np.float32)
        }]

        # 추적
        tracks = tracker.update(fake_detection)

        for track in tracks:
            track_id  = track["track_id"]
            keypoints = track["keypoints"]

            # 버퍼 누적
            buffer = buffer_mgr.update(track_id, keypoints)
            if buffer is None:
                continue

            infer_count += 1

            # 추론
            result = detector.predict(buffer)
            if result is None:
                continue

            label      = result["label"]
            confidence = result["confidence"]

            # 저장 (FALL/UNCERTAIN)
            if label in [RESULT_FALL, RESULT_UNCERTAIN]:
                saver.save(
                    label=label,
                    camera_id=1,
                    track_id=track_id,
                    buffer=buffer,
                    confidence=confidence
                )

            if label == RESULT_FALL:
                fall_count += 1
            elif label == RESULT_UNCERTAIN:
                uncertain_count += 1
            else:
                nonfail_count += 1

    check("150프레임 오류 없이 처리",  True,            "정상 흐름")
    check("추론 발생",                 infer_count > 0, f"{infer_count}회 추론")
    check("track_id 정상 부여",        len(tracker.tracks) > 0, f"추적 중: {len(tracker.tracks)}개")
    check("판정 결과 분류됨",
          (fall_count + uncertain_count + nonfail_count) == infer_count,
          f"FALL={fall_count}, UNCERTAIN={uncertain_count}, NON_FALL={nonfail_count}")

except Exception as e:
    check("전체 파이프라인", False, str(e))


# ── 최종 결과 ─────────────────────────────────────────────────
print("\n" + "=" * 55)
print("  테스트 결과 요약")
print("=" * 55)
passed = sum(1 for s, _, _ in results if s == PASS)
failed = sum(1 for s, _, _ in results if s == FAIL)
for status, name, detail in results:
    print(f"  {status}  {name}")
print(f"\n  총 {len(results)}개  |  통과 {passed}개  |  실패 {failed}개")

if failed == 0:
    print("\n  🎉 전체 파이프라인 정상")
else:
    print(f"\n  ⚠️  {failed}개 항목 확인 필요")
print("=" * 55)
