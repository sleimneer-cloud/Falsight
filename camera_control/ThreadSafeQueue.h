/**
 * @file ThreadSafeQueue.h
 * @brief 스레드 안전한 고정 크기 큐 (링버퍼 방식)
 *
 * 설계 원칙:
 * - Producer-Consumer 패턴 지원
 * - 큐가 가득 차면 오래된 데이터 자동 드롭 (실시간 시스템용)
 * - 안전한 종료 메커니즘 (shutdown)
 *
 * 사용 예시:
 * @code
 *   ThreadSafeQueue<FrameData> queue(30);  // 최대 30개
 *
 *   // Producer
 *   queue.push(frame_data);
 *
 *   // Consumer
 *   FrameData fd;
 *   while (queue.pop(fd)) {
 *       // 처리
 *   }
 *
 *   // 종료
 *   queue.shutdown();
 * @endcode
 */

#ifndef THREAD_SAFE_QUEUE_H
#define THREAD_SAFE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <iostream>

template <typename T>
class ThreadSafeQueue {
public:
    //--------------------------------------------------------------------------
    // 생성자
    //--------------------------------------------------------------------------

    /**
     * @brief 고정 크기 큐 생성
     * @param max_size 최대 저장 개수 (기본 30)
     *
     * 크기 선정 기준:
     * - 15fps × 2초 = 30 프레임
     * - 소비자가 2초간 멈춰도 데이터 유지
     */
    explicit ThreadSafeQueue(size_t max_size = 30)
        : max_size_(max_size)
        , running_(true)
        , dropped_count_(0)
    {
        std::cout << "[QUEUE] 생성 완료 (max_size=" << max_size_ << ")" << std::endl;
    }

    // 복사 금지 (뮤텍스, condition_variable 소유)
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    //--------------------------------------------------------------------------
    // Producer 인터페이스
    //--------------------------------------------------------------------------

    /**
     * @brief 데이터 삽입 (꽉 차면 오래된 것 드롭)
     * @param value 삽입할 데이터
     *
     * @note 블로킹 없음 - 실시간 시스템에서 Producer 지연 방지
     */
    void push(T value) {
        // ★ shutdown 상태면 push 무시
        if (!running_.load()) return;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= max_size_) {
                queue_.pop();
                dropped_count_++;
            }
            queue_.push(std::move(value));
        }
        cond_.notify_one();
    }

    //--------------------------------------------------------------------------
    // Consumer 인터페이스
    //--------------------------------------------------------------------------

    /**
     * @brief 데이터 추출 (없으면 대기)
     * @param value 추출된 데이터를 담을 변수
     * @return true: 데이터 추출 성공, false: 큐 종료됨
     *
     * @note shutdown() 호출 시 false 반환하며 깨어남
     */
    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 데이터가 있거나 종료될 때까지 대기
        cond_.wait(lock, [this] {
            return !queue_.empty() || !running_.load();
            });

        // 종료 상태이고 큐가 비었으면 false
        if (!running_.load() && queue_.empty()) {
            return false;
        }

        // 데이터 추출
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    /**
     * @brief 데이터 추출 (대기 없이)
     * @param value 추출된 데이터를 담을 변수
     * @return true: 데이터 추출 성공, false: 큐가 비어있음
     */
    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            return false;
        }

        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    //--------------------------------------------------------------------------
    // 제어 인터페이스
    //--------------------------------------------------------------------------

    /**
     * @brief 큐 종료 - 대기 중인 모든 스레드 깨움
     *
     * 호출 후:
     * - push()는 계속 가능 (하지만 보통 Producer도 종료)
     * - pop()은 남은 데이터 소진 후 false 반환
     */
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }

        // 대기 중인 모든 Consumer 깨우기
        cond_.notify_all();

        std::cout << "[QUEUE] 종료 신호 전송 (남은 항목: "
            << queue_.size() << ", 총 드롭: "
            << dropped_count_ << ")" << std::endl;
    }

    //--------------------------------------------------------------------------
    // 상태 조회
    //--------------------------------------------------------------------------

    /**
     * @brief 현재 큐 크기
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief 큐가 비어있는지
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /**
     * @brief 드롭된 데이터 개수
     */
    size_t dropped_count() const {
        return dropped_count_.load();
    }

    /**
     * @brief 큐 실행 중 여부
     */
    bool is_running() const {
        return running_.load();
    }

private:
    std::queue<T>           queue_;         // 실제 데이터 저장소
    mutable std::mutex      mutex_;         // 동기화 뮤텍스
    std::condition_variable cond_;          // Consumer 대기용
    const size_t            max_size_;      // 최대 크기
    std::atomic<bool>       running_;       // 실행 상태
    std::atomic<size_t>     dropped_count_; // 드롭 카운터
};

#endif // THREAD_SAFE_QUEUE_H