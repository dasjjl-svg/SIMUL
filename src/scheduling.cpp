#include "scheduling.h"

#include <windows.h>

#include <vector>

namespace loopback {

namespace {

struct Processor {
    PROCESSOR_NUMBER number;
    KAFFINITY groupMask;  // 소속 프로세서 그룹의 활성 프로세서 마스크
};

// 활성 논리 프로세서 전체. 64개 초과 구성의 복수 프로세서 그룹 포함
std::vector<Processor> activeProcessors()
{
    std::vector<Processor> processors;
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationGroup, nullptr, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || !length)
        return processors;
    std::vector<BYTE> buffer(length);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationGroup, info, &length) || info->Relationship != RelationGroup)
        return processors;
    const PROCESSOR_GROUP_INFO* groups = info->Group.GroupInfo;
    for (WORD group = 0; group < info->Group.ActiveGroupCount; ++group) {
        const KAFFINITY mask = groups[group].ActiveProcessorMask;
        for (BYTE bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
            if (!(mask & (static_cast<KAFFINITY>(1) << bit))) continue;
            Processor processor{};
            processor.number.Group = group;
            processor.number.Number = bit;
            processor.groupMask = mask;
            processors.push_back(processor);
        }
    }
    return processors;
}

}  // namespace

void prepareWorkerThread(std::size_t lane)
{
    const HANDLE thread = GetCurrentThread();
    // 대기 위주 스레드. 타 프로그램 부하 중에도 송신 주기 유지
    SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);

    static const std::vector<Processor> processors = activeProcessors();
    if (processors.size() < 2) return;
    const Processor& target = processors[lane % processors.size()];
    GROUP_AFFINITY current{};
    if (GetThreadGroupAffinity(thread, &current) && current.Group != target.number.Group) {
        GROUP_AFFINITY affinity{};
        affinity.Group = target.number.Group;
        affinity.Mask = target.groupMask;
        if (!SetThreadGroupAffinity(thread, &affinity, nullptr)) return;
    }
    PROCESSOR_NUMBER ideal = target.number;
    SetThreadIdealProcessorEx(thread, &ideal, nullptr);
}

}  // namespace loopback
