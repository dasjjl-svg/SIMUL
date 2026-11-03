#pragma once

#include <cstddef>

namespace loopback {

// 시험 작업 스레드 실행 조건: 우선순위 상향, 채널 순번 기준 전 논리 프로세서 순환 배치(프로세서 그룹 포함)
// 작업 스레드 자신이 기동 직후 호출
void prepareWorkerThread(std::size_t lane);

}  // namespace loopback
