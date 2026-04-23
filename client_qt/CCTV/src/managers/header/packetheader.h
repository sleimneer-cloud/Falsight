#ifndef CLIENT_PACKET_HEADER_H
#define CLIENT_PACKET_HEADER_H

#include <cstdint>

#pragma pack(push, 1) // 1바이트 정렬 (OS 간 차이 방지)

// ==============================================================================
// 1. 실시간 스트리밍 수신용 헤더 (ZMQ 포트: 7000번대)
//    VCR -> Client
// ==============================================================================
struct ViewerPacketHeader {
    uint8_t  camera_id;      // 1B : 카메라 번호 (0~3)
    uint8_t  padding[3];     // 3B : 정렬용 (V2: quality_flag 확장 예정)
    uint64_t timestamp_ms;   // 8B : Unix epoch 밀리초
    uint32_t width;          // 4B : 영상 가로 (1920)
    uint32_t height;         // 4B : 영상 세로 (1080)
    uint32_t jpeg_size;      // 4B : 👉 추가됨 (JPEG 페이로드 크기)
};                           // 총 24B

// ==============================================================================
// 2. VCR 제어 명령 송신용 헤더 (TCP 포트: 9000)
//    Client -> VCR (과거 영상 탐색, 재생 제어 등)
// ==============================================================================
struct VcrControlRequest {
    uint16_t message_id;    // 2B : 300 (고정, Big-Endian)
    uint8_t  camera_id;     // 1B : 조회 대상 카메라 ID
    uint8_t  request_type;  // 1B : 재생 요청 유형 코드
    uint64_t start_time;    // 8B : 조회 시작 시각 (Unix timestamp ms)
    uint64_t end_time;      // 8B : 조회 종료 시각 (Unix timestamp ms)
};                          // 총 20B

/**
 * @struct ReqEventListHeader
 * @brief [ID: 310] 이벤트 영상 리스트 요청 (Client -> Main Server) - 20B
 */

struct ReqEventListHeader {
    uint16_t message_id;    // 2B : 310 (고정, Big-Endian)
    uint8_t  camera_id;     // 1B : 조회 대상 카메라 ID (0xFF=전체)
    uint8_t  reserved;      // 1B : 예약 (0x00)
    uint64_t start_time;    // 8B : 조회 시작 시각 (Unix timestamp ms)
    uint64_t end_time;      // 8B : 조회 종료 시각 (Unix timestamp ms)
};

/**
 * @struct ReqEventVideoHeader
 * @brief [ID: 302] 이벤트 영상(URL) 요청 (Client -> Main Server) - 20B
 */

struct ReqEventVideoHeader {
    uint16_t message_id;    // 2B : 302 (고정)
    uint8_t  camera_id;     // 1B : 조회 대상 카메라 ID
    uint8_t  event_type;    // 1B : 이벤트 유형 코드 (0x01=낙상)
    uint64_t event_id;      // 8B : 조회 대상 이벤트 ID
    uint64_t timestamp;     // 8B : 요청 시각
};

/**
 * @struct ResMainServerHeader
 * @brief [ID: 311, 303] 메인 서버 공통 응답 헤더 (Main Server -> Client) - 20B
 * ※ 이 20바이트 뒤에 payload_size 만큼의 가변 데이터(JSON 등)가 붙습니다.
 */
struct ResMainServerHeader {
    uint16_t message_id;    // 2B : 311 또는 303
    uint8_t  status_code;   // 1B : 0x00=성공, 0x01=없음, 0x02=에러
    uint8_t  reserved[13];  // 13B: 예약 (0 패딩)
    uint32_t payload_size;  // 4B : 뒤따라오는 가변 데이터의 크기
};

#pragma pack(pop)

// 👉 크기 검증도 24바이트로 변경
static_assert(sizeof(ViewerPacketHeader) == 24, "ViewerPacketHeader must be exactly 24 bytes");
static_assert(sizeof(VcrControlRequest) == 20, "VcrControlRequest must be exactly 20 bytes");

static_assert(sizeof(ReqEventListHeader) == 20, "Size Error: 310");
static_assert(sizeof(ReqEventVideoHeader) == 20, "Size Error: 302");
static_assert(sizeof(ResMainServerHeader) == 20, "Size Error: 311/303");

#endif // CLIENT_PACKET_HEADER_H