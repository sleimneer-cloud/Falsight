/**
 * @file PacketHeader.h
 * @brief ZMQ 전송용 패킷 헤더 및 공통 프레임 데이터 구조체 정의
 */

#ifndef PACKET_HEADER_H
#define PACKET_HEADER_H

#include <cstdint>
#include <opencv2/opencv.hpp> // ★ 추가: FrameData 내부의 cv::Mat을 사용하기 위해 필수!

 //==============================================================================
 // ★ 추가: 스레드 간 큐(Queue)에서 주고받을 공통 프레임 데이터 구조체
 //==============================================================================
struct FrameData {
    int      camera_id;
    int64_t  timestamp_ms;
    uint32_t frame_id;
    cv::Mat  raw;          // 원본 1920x1080
    cv::Mat  resized;      // 리사이즈 640x480
    bool     has_motion;   // 모션 감지 여부
};

//==============================================================================
// 네트워크 패킷 헤더 (1바이트 정렬)
//==============================================================================
#pragma pack(push, 1)  // 1바이트 정렬 (패딩 방지)

 /**
  * @struct AIPacketHeader
  * @brief AI 서버 전송용 헤더 (20 바이트)
  */
struct AIPacketHeader {
    uint8_t  camera_id;      // 1B : 카메라 번호 (0~3)
    uint8_t  padding[3];     // 3B : 정렬용 (V2: quality_flag 확장 예정)
    uint64_t timestamp_ms;   // 8B : Unix epoch 밀리초
    uint32_t frame_id;       // 4B : 프레임 순번 (유실 감지용)
    uint32_t jpeg_size;      // 4B : JPEG 페이로드 크기
};                           // 총 20B

/**
 * @struct ViewerPacketHeader
 * @brief 클라이언트 스트리밍용 헤더 (20 바이트)
 */
struct ViewerPacketHeader {
    uint8_t  camera_id;      // 1B : 카메라 번호 (0~3)
    uint8_t  padding[3];     // 3B : 정렬용 (V2: quality_flag 확장 예정)
    uint64_t timestamp_ms;   // 8B : Unix epoch 밀리초
    uint32_t width;          // 4B : 영상 가로 (1920)
    uint32_t height;         // 4B : 영상 세로 (1080)
    uint32_t jpeg_size;
};                           // 총 24B

#pragma pack(pop)

//==============================================================================
// 컴파일 타임 검증
//==============================================================================

static_assert(sizeof(AIPacketHeader) == 20,
    "AIPacketHeader must be exactly 20 bytes");

static_assert(sizeof(ViewerPacketHeader) == 24,
    "ViewerPacketHeader must be exactly 24 bytes");

//==============================================================================
// 상수 정의
//==============================================================================

namespace PacketConfig {
    // 포트 설정
    constexpr uint16_t AI_SERVER_PORT = 9001;       // AI 서버 포트
    constexpr uint16_t CONTROL_PORT = 9000;         // 제어 서버 포트
    constexpr uint16_t HTTP_PORT = 8080;            // HTTP 서버 포트

    // JPEG 설정
    constexpr int JPEG_QUALITY = 85;             // JPEG 압축 품질
    constexpr int AI_WIDTH = 640;                // AI용 해상도
    constexpr int AI_HEIGHT = 480;
    constexpr int VIEWER_WIDTH = 1920;           // 클라이언트용 해상도
    constexpr int VIEWER_HEIGHT = 1080;


    // 버퍼 크기
    constexpr size_t MAX_JPEG_SIZE = 500 * 1024; // 최대 JPEG 크기 (500KB)
    constexpr size_t HEADER_SIZE = 20;           // 헤더 크기
}

#endif // PACKET_HEADER_H