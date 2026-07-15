#include "avm/mem/phys_mem.h"

#include <algorithm>
#include <cstring>

#include "avm/log.h"

namespace avm {

namespace {
constexpr const char* kTag = "avm.mem";

bool RangesOverlap(GuestAddr a_base, u64 a_size, GuestAddr b_base, u64 b_size) {
    return a_base < b_base + b_size && b_base < a_base + a_size;
}
} // namespace

bool PhysMem::AddBackedRegion(GuestAddr base, u64 size, const char* name, bool writable) {
    if (size == 0 || (base & kGuestPageMask) != 0 || (size & kGuestPageMask) != 0) {
        AVM_LOGE(kTag, "%s: 페이지 정렬 위반 base=0x%llx size=0x%llx", name,
                 (unsigned long long)base, (unsigned long long)size);
        return false;
    }
    if (base + size < base) {
        AVM_LOGE(kTag, "%s: 주소 오버플로", name);
        return false;
    }
    for (const auto& r : region_infos_) {
        if (RangesOverlap(base, size, r.base, r.size)) {
            AVM_LOGE(kTag, "%s: 기존 영역 %s와 겹침", name, r.name);
            return false;
        }
    }

    auto region = std::make_unique<BackedRegion>();
    region->info.base = base;
    region->info.size = size;
    region->info.name = name;
    region->info.is_ram = true;
    region->info.writable = writable;
    region->data = std::make_unique<u8[]>(size); // 0으로 초기화됨
    region->pages.resize(size >> kGuestPageShift);
    const u8 flags = writable ? static_cast<u8>(kPageRwx)
                              : static_cast<u8>(kPageRead | kPageExec);
    for (auto& page : region->pages) page.flags = flags;

    region_infos_.push_back(region->info);
    regions_.push_back(std::move(region));
    last_region_ = nullptr;
    return true;
}

bool PhysMem::AddRam(GuestAddr base, u64 size, const char* name) {
    return AddBackedRegion(base, size, name, /*writable=*/true);
}

bool PhysMem::AddRom(GuestAddr base, u64 size, const char* name) {
    return AddBackedRegion(base, size, name, /*writable=*/false);
}

bool PhysMem::AddDevice(GuestAddr base, u64 size, DeviceRegion* device) {
    if (device == nullptr || size == 0) return false;
    for (const auto& r : region_infos_) {
        if (RangesOverlap(base, size, r.base, r.size)) {
            AVM_LOGE(kTag, "장치 %s: 기존 영역 %s와 겹침", device->Name(), r.name);
            return false;
        }
    }
    auto region = std::make_unique<BackedRegion>();
    region->info.base = base;
    region->info.size = size;
    region->info.name = device->Name();
    region->info.is_ram = false;
    region->info.writable = true;
    region->info.device = device;

    region_infos_.push_back(region->info);
    regions_.push_back(std::move(region));
    last_region_ = nullptr;
    return true;
}

PhysMem::BackedRegion* PhysMem::FindRegion(GuestAddr addr) {
    // 단일 vCPU 단계의 단순 캐시. 멀티 vCPU(Stage 11)에서는 vCPU별
    // TLB/영역 캐시로 대체된다.
    if (last_region_ != nullptr) {
        const auto& info = last_region_->info;
        if (addr - info.base < info.size) return last_region_;
    }
    for (auto& region : regions_) {
        if (addr - region->info.base < region->info.size) {
            last_region_ = region.get();
            return last_region_;
        }
    }
    return nullptr;
}

MemFault PhysMem::Read(GuestAddr addr, void* dst, u64 len) {
    u8* out = static_cast<u8*>(dst);
    while (len > 0) {
        BackedRegion* region = FindRegion(addr);
        if (region == nullptr) {
            return MemFault{MemFaultKind::kUnmapped, addr, /*is_write=*/false, false};
        }
        const u64 offset = addr - region->info.base;
        const u64 chunk = std::min<u64>(len, region->info.size - offset);
        if (region->info.is_ram) {
            std::memcpy(out, region->data.get() + offset, chunk);
        } else {
            // MMIO: 자연 정렬된 1/2/4/8바이트 접근만 허용.
            if (chunk != len || (len != 1 && len != 2 && len != 4 && len != 8) ||
                (addr & (len - 1)) != 0) {
                return MemFault{MemFaultKind::kMmioError, addr, false, false};
            }
            u64 value = 0;
            if (!region->info.device->MmioRead(offset, static_cast<unsigned>(len), value)) {
                return MemFault{MemFaultKind::kMmioError, addr, false, false};
            }
            std::memcpy(out, &value, len);
            return MemFault{};
        }
        addr += chunk;
        out += chunk;
        len -= chunk;
    }
    return MemFault{};
}

MemFault PhysMem::Write(GuestAddr addr, const void* src, u64 len) {
    const u8* in = static_cast<const u8*>(src);
    while (len > 0) {
        BackedRegion* region = FindRegion(addr);
        if (region == nullptr) {
            return MemFault{MemFaultKind::kUnmapped, addr, /*is_write=*/true, false};
        }
        const u64 offset = addr - region->info.base;
        const u64 chunk = std::min<u64>(len, region->info.size - offset);
        if (region->info.is_ram) {
            if (!region->info.writable) {
                return MemFault{MemFaultKind::kPermission, addr, true, false};
            }
            std::memcpy(region->data.get() + offset, in, chunk);
            // dirty 및 코드 무효화 추적.
            const u64 first_page = offset >> kGuestPageShift;
            const u64 last_page = (offset + chunk - 1) >> kGuestPageShift;
            for (u64 p = first_page; p <= last_page; ++p) {
                PageInfo& page = region->pages[p];
                page.flags |= kPageDirty;
                if (page.flags & kPageHasCode) {
                    // 게스트 코드 자체 수정: 세대 번호를 올려 JIT가 재번역하게 한다.
                    page.code_generation++;
                    page.flags &= static_cast<u8>(~kPageHasCode);
                }
            }
        } else {
            if (chunk != len || (len != 1 && len != 2 && len != 4 && len != 8) ||
                (addr & (len - 1)) != 0) {
                return MemFault{MemFaultKind::kMmioError, addr, true, false};
            }
            u64 value = 0;
            std::memcpy(&value, in, len);
            if (!region->info.device->MmioWrite(offset, static_cast<unsigned>(len), value)) {
                return MemFault{MemFaultKind::kMmioError, addr, true, false};
            }
            return MemFault{};
        }
        addr += chunk;
        in += chunk;
        len -= chunk;
    }
    return MemFault{};
}

MemFault PhysMem::FetchU32(GuestAddr addr, u32& inst) {
    BackedRegion* region = FindRegion(addr);
    if (region == nullptr || !region->info.is_ram) {
        // MMIO 영역에서의 실행은 지원하지 않는다.
        return MemFault{MemFaultKind::kUnmapped, addr, false, /*is_fetch=*/true};
    }
    const u64 offset = addr - region->info.base;
    if (offset + 4 > region->info.size) {
        return MemFault{MemFaultKind::kUnmapped, addr, false, true};
    }
    const PageInfo& page = region->pages[offset >> kGuestPageShift];
    if (!(page.flags & kPageExec)) {
        return MemFault{MemFaultKind::kPermission, addr, false, true};
    }
    std::memcpy(&inst, region->data.get() + offset, 4);
    return MemFault{};
}

bool PhysMem::LoadImage(GuestAddr addr, const void* data, u64 len) {
    // ROM에도 적재할 수 있어야 하므로 Write()의 권한 검사를 우회한다.
    u64 remaining = len;
    const u8* in = static_cast<const u8*>(data);
    GuestAddr cur = addr;
    // 먼저 전체 범위가 백킹 메모리인지 확인 (부분 적재 방지).
    while (remaining > 0) {
        BackedRegion* region = FindRegion(cur);
        if (region == nullptr || !region->info.is_ram) return false;
        const u64 offset = cur - region->info.base;
        const u64 chunk = std::min<u64>(remaining, region->info.size - offset);
        cur += chunk;
        remaining -= chunk;
    }
    remaining = len;
    cur = addr;
    while (remaining > 0) {
        BackedRegion* region = FindRegion(cur);
        const u64 offset = cur - region->info.base;
        const u64 chunk = std::min<u64>(remaining, region->info.size - offset);
        std::memcpy(region->data.get() + offset, in, chunk);
        cur += chunk;
        in += chunk;
        remaining -= chunk;
    }
    return true;
}

u8* PhysMem::HostPtr(GuestAddr addr, u64* contiguous) {
    BackedRegion* region = FindRegion(addr);
    if (region == nullptr || !region->info.is_ram) return nullptr;
    const u64 offset = addr - region->info.base;
    if (contiguous != nullptr) *contiguous = region->info.size - offset;
    return region->data.get() + offset;
}

PageInfo* PhysMem::Page(GuestAddr addr) {
    BackedRegion* region = FindRegion(addr);
    if (region == nullptr || !region->info.is_ram) return nullptr;
    return &region->pages[(addr - region->info.base) >> kGuestPageShift];
}

void PhysMem::MarkPageHasCode(GuestAddr addr) {
    if (PageInfo* page = Page(addr)) {
        page->flags |= kPageHasCode;
    }
}

} // namespace avm
