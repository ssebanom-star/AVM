// 게스트 물리 주소 공간.
//
// 설계 원칙:
//  - RAM은 호스트의 연속 버퍼로 백킹하되, 4KB 페이지 단위 메타데이터를
//    별도 배열로 유지한다 (코드 번역 추적, dirty, 권한).
//  - MMIO는 DeviceRegion 인터페이스로 분리하여 느린 경로로 처리한다.
//    (JIT의 인라인 빠른 메모리 접근은 RAM에만 적용되고, MMIO는 항상
//     헬퍼 함수 호출로 빠진다)
//  - 접근 실패는 예외를 던지지 않고 MemFault로 반환한다. 게스트의
//    페이지 폴트는 정상 제어 흐름이기 때문이다.
#pragma once

#include <memory>
#include <vector>

#include "avm/cpu/exception.h"
#include "avm/types.h"

namespace avm {

// 페이지 메타데이터 플래그.
enum PageFlags : u8 {
    kPageRead      = 1 << 0,
    kPageWrite     = 1 << 1,
    kPageExec      = 1 << 2,
    kPageDirty     = 1 << 3, // 마지막 스냅샷/플러시 이후 쓰기 발생
    kPageHasCode   = 1 << 4, // 이 페이지에서 번역 블록이 생성됨(JIT 무효화용)
    kPageRwx = kPageRead | kPageWrite | kPageExec,
};

struct PageInfo {
    u8 flags = 0;
    // 코드 무효화 세대 번호: 페이지의 코드가 수정될 때마다 증가.
    // JIT(Stage 4)는 번역 블록에 세대를 기록해 값이 다르면 재번역한다.
    u32 code_generation = 0;
};

// MMIO 장치가 구현하는 인터페이스 (Stage 2의 UART부터 사용).
// 반환값 false는 장치 수준 접근 오류(MemFaultKind::kMmioError)를 뜻한다.
class DeviceRegion {
public:
    virtual ~DeviceRegion() = default;
    virtual const char* Name() const = 0;
    // offset: 영역 시작 기준. size: 1/2/4/8.
    virtual bool MmioRead(u64 offset, unsigned size, u64& value) = 0;
    virtual bool MmioWrite(u64 offset, unsigned size, u64 value) = 0;
};

class PhysMem {
public:
    PhysMem() = default;
    PhysMem(const PhysMem&) = delete;
    PhysMem& operator=(const PhysMem&) = delete;

    // RAM/ROM 영역 매핑. 겹치는 영역이 있으면 false.
    // base와 size는 페이지 정렬이어야 한다.
    bool AddRam(GuestAddr base, u64 size, const char* name = "ram");
    bool AddRom(GuestAddr base, u64 size, const char* name = "rom");
    // MMIO 장치 매핑. device는 호출자가 소유하며 PhysMem보다 오래 살아야 한다.
    bool AddDevice(GuestAddr base, u64 size, DeviceRegion* device);

    // ------------------------------------------------------------------
    // 데이터 접근 (게스트 물리 주소)
    // ------------------------------------------------------------------
    // 성공 시 fault.kind == kNone. 실패 시 어떤 바이트도 부분 반영하지 않는
    // 것을 보장하지는 않지만(영역 경계를 걸치는 접근), 실패 주소는 정확히
    // 보고한다. 실제 게스트는 정상적으로는 영역 경계를 걸치는 단일 접근을
    // 만들지 않는다.
    MemFault Read(GuestAddr addr, void* dst, u64 len);
    MemFault Write(GuestAddr addr, const void* src, u64 len);

    // 정수 접근 헬퍼 (리틀 엔디언 — AArch64 게스트 기본).
    template <typename T>
    MemFault ReadT(GuestAddr addr, T& out) {
        return Read(addr, &out, sizeof(T));
    }
    template <typename T>
    MemFault WriteT(GuestAddr addr, T value) {
        return Write(addr, &value, sizeof(T));
    }

    // 명령어 인출: 실행 권한을 검사하며 is_fetch를 보고한다.
    MemFault FetchU32(GuestAddr addr, u32& inst);

    // 게스트 이미지 적재 (펌웨어/테스트 바이너리). ROM에도 쓸 수 있다.
    bool LoadImage(GuestAddr addr, const void* data, u64 len);

    // ------------------------------------------------------------------
    // 페이지 메타데이터 / 빠른 경로 지원
    // ------------------------------------------------------------------
    // addr가 RAM/ROM에 속하면 해당 호스트 포인터를 반환하고 남은 연속
    // 바이트 수를 contiguous에 채운다. MMIO/미매핑이면 nullptr.
    // (JIT의 게스트 RAM 호스트 포인터 캐시가 이 API를 사용한다)
    u8* HostPtr(GuestAddr addr, u64* contiguous = nullptr);

    PageInfo* Page(GuestAddr addr);
    // 페이지에 번역된 코드가 있음을 표시 (Stage 4).
    void MarkPageHasCode(GuestAddr addr);

    struct RegionInfo {
        GuestAddr base = 0;
        u64 size = 0;
        const char* name = "";
        bool is_ram = false;   // 백킹 메모리 존재 (ROM 포함)
        bool writable = false; // ROM이면 false
        DeviceRegion* device = nullptr;
    };
    const std::vector<RegionInfo>& Regions() const { return region_infos_; }

private:
    struct BackedRegion {
        RegionInfo info;
        std::unique_ptr<u8[]> data;          // is_ram일 때만
        std::vector<PageInfo> pages;         // is_ram일 때만
    };

    BackedRegion* FindRegion(GuestAddr addr);
    bool AddBackedRegion(GuestAddr base, u64 size, const char* name, bool writable);

    std::vector<std::unique_ptr<BackedRegion>> regions_;
    std::vector<RegionInfo> region_infos_;
    // 마지막으로 적중한 영역 캐시 (연속 접근 최적화).
    BackedRegion* last_region_ = nullptr;
};

} // namespace avm
