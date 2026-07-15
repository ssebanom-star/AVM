// 게스트 물리 메모리 단위 테스트.
#include "avm/board.h"
#include "avm/mem/phys_mem.h"
#include "test_framework.h"

using namespace avm;

TEST(Mem_MapAndRoundTrip) {
    PhysMem mem;
    CHECK(mem.AddRam(board::kRamBase, 64 * 1024));
    CHECK(mem.WriteT<u64>(board::kRamBase + 8, 0x1122334455667788ull).kind ==
          MemFaultKind::kNone);
    u64 value = 0;
    CHECK(mem.ReadT(board::kRamBase + 8, value).kind == MemFaultKind::kNone);
    CHECK_EQ(value, 0x1122334455667788ull);
    // 리틀 엔디언 바이트 배치 확인.
    u8 byte = 0;
    mem.ReadT(board::kRamBase + 8, byte);
    CHECK_EQ(byte, 0x88);
}

TEST(Mem_RejectsBadMappings) {
    PhysMem mem;
    CHECK(!mem.AddRam(board::kRamBase + 1, 4096));      // 정렬 위반
    CHECK(!mem.AddRam(board::kRamBase, 100));           // 크기 정렬 위반
    CHECK(mem.AddRam(board::kRamBase, 8192));
    CHECK(!mem.AddRam(board::kRamBase + 4096, 8192));   // 겹침
}

TEST(Mem_UnmappedFault) {
    PhysMem mem;
    mem.AddRam(board::kRamBase, 4096);
    u32 value = 0;
    MemFault fault = mem.ReadT(0x1000, value);
    CHECK(fault.kind == MemFaultKind::kUnmapped);
    CHECK_EQ(fault.addr, 0x1000ull);
    CHECK(!fault.is_write);
    fault = mem.WriteT<u32>(board::kRamBase + 4096, 1); // 영역 끝 바로 다음
    CHECK(fault.kind == MemFaultKind::kUnmapped);
}

TEST(Mem_RomIsNotWritable) {
    PhysMem mem;
    CHECK(mem.AddRom(board::kFlashCodeBase, 4096, "flash"));
    const MemFault fault = mem.WriteT<u32>(board::kFlashCodeBase, 1);
    CHECK(fault.kind == MemFaultKind::kPermission);
    // LoadImage는 ROM에도 쓸 수 있다 (펌웨어 적재 경로).
    const u32 image = 0xDEADBEEF;
    CHECK(mem.LoadImage(board::kFlashCodeBase, &image, 4));
    u32 value = 0;
    CHECK(mem.ReadT(board::kFlashCodeBase, value).kind == MemFaultKind::kNone);
    CHECK_EQ(value, 0xDEADBEEFu);
}

TEST(Mem_CrossPageAccess) {
    PhysMem mem;
    mem.AddRam(board::kRamBase, 8192);
    // 페이지 경계를 걸치는 8바이트 쓰기/읽기.
    const GuestAddr addr = board::kRamBase + 4096 - 4;
    CHECK(mem.WriteT<u64>(addr, 0xAABBCCDD11223344ull).kind == MemFaultKind::kNone);
    u64 value = 0;
    CHECK(mem.ReadT(addr, value).kind == MemFaultKind::kNone);
    CHECK_EQ(value, 0xAABBCCDD11223344ull);
    // 양쪽 페이지 모두 dirty.
    CHECK(mem.Page(board::kRamBase)->flags & kPageDirty);
    CHECK(mem.Page(board::kRamBase + 4096)->flags & kPageDirty);
}

TEST(Mem_FetchChecksExecPermission) {
    PhysMem mem;
    mem.AddRam(board::kRamBase, 8192);
    const u32 inst = 0xD503201F;
    mem.LoadImage(board::kRamBase, &inst, 4);
    u32 fetched = 0;
    CHECK(mem.FetchU32(board::kRamBase, fetched).kind == MemFaultKind::kNone);
    CHECK_EQ(fetched, inst);
    // 실행 권한 제거 후 인출은 권한 폴트.
    mem.Page(board::kRamBase)->flags &= static_cast<u8>(~kPageExec);
    const MemFault fault = mem.FetchU32(board::kRamBase, fetched);
    CHECK(fault.kind == MemFaultKind::kPermission);
    CHECK(fault.is_fetch);
}

TEST(Mem_CodeGenerationInvalidation) {
    PhysMem mem;
    mem.AddRam(board::kRamBase, 4096);
    mem.MarkPageHasCode(board::kRamBase);
    CHECK(mem.Page(board::kRamBase)->flags & kPageHasCode);
    const u32 gen = mem.Page(board::kRamBase)->code_generation;
    mem.WriteT<u32>(board::kRamBase + 100, 0x12345678);
    CHECK_EQ(mem.Page(board::kRamBase)->code_generation, gen + 1);
    CHECK(!(mem.Page(board::kRamBase)->flags & kPageHasCode));
    // 코드 없는 페이지에 쓰기는 세대를 올리지 않는다.
    mem.WriteT<u32>(board::kRamBase + 200, 1);
    CHECK_EQ(mem.Page(board::kRamBase)->code_generation, gen + 1);
}

TEST(Mem_HostPtrFastPath) {
    PhysMem mem;
    mem.AddRam(board::kRamBase, 8192);
    u64 contiguous = 0;
    u8* ptr = mem.HostPtr(board::kRamBase + 100, &contiguous);
    CHECK(ptr != nullptr);
    CHECK_EQ(contiguous, 8192ull - 100);
    // 호스트 포인터로 쓴 값이 게스트 읽기로 보인다.
    ptr[0] = 0x42;
    u8 value = 0;
    mem.ReadT(board::kRamBase + 100, value);
    CHECK_EQ(value, 0x42);
    CHECK(mem.HostPtr(0x123, nullptr) == nullptr);
}

namespace {
// MMIO 장치 스텁: 마지막 쓰기를 저장하고, 읽기는 offset을 되돌려준다.
class StubDevice : public DeviceRegion {
public:
    const char* Name() const override { return "stub"; }
    bool MmioRead(u64 offset, unsigned size, u64& value) override {
        last_read_size = size;
        value = offset;
        return true;
    }
    bool MmioWrite(u64 offset, unsigned size, u64 value) override {
        last_write_offset = offset;
        last_write_value = value;
        last_write_size = size;
        return true;
    }
    u64 last_write_offset = 0;
    u64 last_write_value = 0;
    unsigned last_write_size = 0;
    unsigned last_read_size = 0;
};
} // namespace

TEST(Mem_MmioDispatch) {
    PhysMem mem;
    StubDevice dev;
    CHECK(mem.AddDevice(board::kUart0Base, board::kUart0Size, &dev));
    CHECK(mem.WriteT<u32>(board::kUart0Base + 0x18, 0x77).kind ==
          MemFaultKind::kNone);
    CHECK_EQ(dev.last_write_offset, 0x18ull);
    CHECK_EQ(dev.last_write_value, 0x77ull);
    CHECK_EQ(dev.last_write_size, 4u);
    u32 value = 0;
    CHECK(mem.ReadT(board::kUart0Base + 0x24, value).kind == MemFaultKind::kNone);
    CHECK_EQ(value, 0x24u);
    // 비정렬 MMIO 접근은 거부.
    u16 half = 0;
    CHECK(mem.ReadT(board::kUart0Base + 0x19, half).kind ==
          MemFaultKind::kMmioError);
    // MMIO에서의 명령어 인출은 거부.
    u32 inst = 0;
    CHECK(mem.FetchU32(board::kUart0Base, inst).kind != MemFaultKind::kNone);
}
