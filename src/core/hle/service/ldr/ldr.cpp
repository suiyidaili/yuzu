// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <memory>
#include <fmt/format.h>
#include <mbedtls/sha256.h>

#include "common/alignment.h"
#include "common/hex_util.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/memory/page_table.h"
#include "core/hle/kernel/memory/system_control.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/service/ldr/ldr.h"
#include "core/hle/service/service.h"
#include "core/loader/nro.h"
#include "core/memory.h"

namespace Service::LDR {

constexpr ResultCode ERROR_INSUFFICIENT_ADDRESS_SPACE{ErrorModule::RO, 2};

[[maybe_unused]] constexpr ResultCode ERROR_INVALID_MEMORY_STATE{ErrorModule::Loader, 51};
constexpr ResultCode ERROR_INVALID_NRO{ErrorModule::Loader, 52};
constexpr ResultCode ERROR_INVALID_NRR{ErrorModule::Loader, 53};
constexpr ResultCode ERROR_MISSING_NRR_HASH{ErrorModule::Loader, 54};
constexpr ResultCode ERROR_MAXIMUM_NRO{ErrorModule::Loader, 55};
constexpr ResultCode ERROR_MAXIMUM_NRR{ErrorModule::Loader, 56};
constexpr ResultCode ERROR_ALREADY_LOADED{ErrorModule::Loader, 57};
constexpr ResultCode ERROR_INVALID_ALIGNMENT{ErrorModule::Loader, 81};
constexpr ResultCode ERROR_INVALID_SIZE{ErrorModule::Loader, 82};
constexpr ResultCode ERROR_INVALID_NRO_ADDRESS{ErrorModule::Loader, 84};
[[maybe_unused]] constexpr ResultCode ERROR_INVALID_NRR_ADDRESS{ErrorModule::Loader, 85};
constexpr ResultCode ERROR_NOT_INITIALIZED{ErrorModule::Loader, 87};

constexpr std::size_t MAXIMUM_LOADED_RO{0x40};
constexpr std::size_t MAXIMUM_MAP_RETRIES{0x200};

constexpr std::size_t TEXT_INDEX{0};
constexpr std::size_t RO_INDEX{1};
constexpr std::size_t DATA_INDEX{2};

struct NRRCertification {
    u64_le application_id_mask;
    u64_le application_id_pattern;
    INSERT_PADDING_BYTES(0x10);
    std::array<u8, 0x100> public_key; // Also known as modulus
    std::array<u8, 0x100> signature;
};
static_assert(sizeof(NRRCertification) == 0x220, "NRRCertification has invalid size.");

struct NRRHeader {
    u32_le magic;
    u32_le certification_signature_key_generation; // 9.0.0+
    INSERT_PADDING_WORDS(2);
    NRRCertification certification;
    std::array<u8, 0x100> signature;
    u64_le application_id;
    u32_le size;
    u8 nrr_kind; // 7.0.0+
    INSERT_PADDING_BYTES(3);
    u32_le hash_offset;
    u32_le hash_count;
    INSERT_PADDING_WORDS(2);
};
static_assert(sizeof(NRRHeader) == 0x350, "NRRHeader has invalid size.");

struct SegmentHeader {
    u32_le memory_offset;
    u32_le memory_size;
};
static_assert(sizeof(SegmentHeader) == 0x8, "SegmentHeader has invalid size.");

struct NROHeader {
    // Switchbrew calls this "Start" (0x10)
    INSERT_PADDING_WORDS(1);
    u32_le mod_offset;
    INSERT_PADDING_WORDS(2);

    // Switchbrew calls this "Header" (0x70)
    u32_le magic;
    u32_le version;
    u32_le nro_size;
    u32_le flags;
    // .text, .ro, .data
    std::array<SegmentHeader, 3> segment_headers;
    u32_le bss_size;
    INSERT_PADDING_WORDS(1);
    std::array<u8, 0x20> build_id;
    u32_le dso_handle_offset;
    INSERT_PADDING_WORDS(1);
    // .apiInfo, .dynstr, .dynsym
    std::array<SegmentHeader, 3> segment_headers_2;
};
static_assert(sizeof(NROHeader) == 0x80, "NROHeader has invalid size.");

using SHA256Hash = std::array<u8, 0x20>;

struct NROInfo {
    SHA256Hash hash{};
    VAddr nro_address{};
    std::size_t nro_size{};
    VAddr bss_address{};
    std::size_t bss_size{};
    std::size_t text_size{};
    std::size_t ro_size{};
    std::size_t data_size{};
    VAddr src_addr{};
};
static_assert(sizeof(NROInfo) == 0x60, "NROInfo has invalid size.");

class DebugMonitor final : public ServiceFramework<DebugMonitor> {
public:
    explicit DebugMonitor(Core::System& system_) : ServiceFramework{system_, "ldr:dmnt"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "AddProcessToDebugLaunchQueue"},
            {1, nullptr, "ClearDebugLaunchQueue"},
            {2, nullptr, "GetNsoInfos"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class ProcessManager final : public ServiceFramework<ProcessManager> {
public:
    explicit ProcessManager(Core::System& system_) : ServiceFramework{system_, "ldr:pm"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "CreateProcess"},
            {1, nullptr, "GetProgramInfo"},
            {2, nullptr, "RegisterTitle"},
            {3, nullptr, "UnregisterTitle"},
            {4, nullptr, "SetEnabledProgramVerification"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class Shell final : public ServiceFramework<Shell> {
public:
    explicit Shell(Core::System& system_) : ServiceFramework{system_, "ldr:shel"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "AddProcessToLaunchQueue"},
            {1, nullptr, "ClearLaunchQueue"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class RelocatableObject final : public ServiceFramework<RelocatableObject> {
public:
    explicit RelocatableObject(Core::System& system_) : ServiceFramework{system_, "ldr:ro"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &RelocatableObject::LoadNro, "LoadNro"},
            {1, &RelocatableObject::UnloadNro, "UnloadNro"},
            {2, &RelocatableObject::LoadNrr, "LoadNrr"},
            {3, &RelocatableObject::UnloadNrr, "UnloadNrr"},
            {4, &RelocatableObject::Initialize, "Initialize"},
            {10, nullptr, "LoadNrrEx"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

    void LoadNrr(Kernel::HLERequestContext& ctx) {
        struct Parameters {
            u64_le process_id;
            u64_le nrr_address;
            u64_le nrr_size;
        };

        IPC::RequestParser rp{ctx};
        const auto [process_id, nrr_address, nrr_size] = rp.PopRaw<Parameters>();

        LOG_DEBUG(Service_LDR,
                  "called with process_id={:016X}, nrr_address={:016X}, nrr_size={:016X}",
                  process_id, nrr_address, nrr_size);

        if (!initialized) {
            LOG_ERROR(Service_LDR, "LDR:RO not initialized before use!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_NOT_INITIALIZED);
            return;
        }

        if (nrr.size() >= MAXIMUM_LOADED_RO) {
            LOG_ERROR(Service_LDR, "Loading new NRR would exceed the maximum number of loaded NRRs "
                                   "(0x40)! Failing...");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_MAXIMUM_NRR);
            return;
        }

        // NRR Address does not fall on 0x1000 byte boundary
        if (!Common::Is4KBAligned(nrr_address)) {
            LOG_ERROR(Service_LDR, "NRR Address has invalid alignment (actual {:016X})!",
                      nrr_address);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_INVALID_ALIGNMENT);
            return;
        }

        // NRR Size is zero or causes overflow
        if (nrr_address + nrr_size <= nrr_address || nrr_size == 0 ||
            !Common::Is4KBAligned(nrr_size)) {
            LOG_ERROR(Service_LDR, "NRR Size is invalid! (nrr_address={:016X}, nrr_size={:016X})",
                      nrr_address, nrr_size);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_INVALID_SIZE);
            return;
        }

        // Read NRR data from memory
        std::vector<u8> nrr_data(nrr_size);
        system.Memory().ReadBlock(nrr_address, nrr_data.data(), nrr_size);
        NRRHeader header;
        std::memcpy(&header, nrr_data.data(), sizeof(NRRHeader));

        if (header.magic != Common::MakeMagic('N', 'R', 'R', '0')) {
            LOG_ERROR(Service_LDR, "NRR did not have magic 'NRR0' (actual {:08X})!", header.magic);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_INVALID_NRR);
            return;
        }

        if (header.size != nrr_size) {
            LOG_ERROR(Service_LDR,
                      "NRR header reported size did not match LoadNrr parameter size! "
                      "(header_size={:016X}, loadnrr_size={:016X})",
                      header.size, nrr_size);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_INVALID_SIZE);
            return;
        }

        if (system.CurrentProcess()->GetTitleID() != header.application_id) {
            LOG_ERROR(Service_LDR,
                      "Attempting to load NRR with title ID other than current process. (actual "
                      "{:016X})!",
                      header.application_id);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_INVALID_NRR);
            return;
        }

        std::vector<SHA256Hash> hashes;

        // Copy all hashes in the NRR (specified by hash count/hash offset) into vector.
        for (std::size_t i = header.hash_offset;
             i < (header.hash_offset + (header.hash_count * sizeof(SHA256Hash))); i += 8) {
            SHA256Hash hash;
            std::memcpy(hash.data(), nrr_data.data() + i, sizeof(SHA256Hash));
            hashes.emplace_back(hash);
        }

        nrr.insert_or_assign(nrr_address, std::move(hashes));

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(RESULT_SUCCESS);
    }

    void UnloadNrr(Kernel::HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto pid = rp.Pop<u64>();
        const auto nrr_address = rp.Pop<VAddr>();

        LOG_DEBUG(Service_LDR, "called with pid={}, nrr_address={:016X}", pid, nrr_address);

        nrr.erase(nrr_address);

        IPC::ResponseBuilder rb{ctx, 2};

        rb.Push(RESULT_SUCCESS);
    }

    bool ValidateRegionForMap(Kernel::Memory::PageTable& page_table, VAddr start,
                              std::size_t size) const {
        constexpr std::size_t padding_size{4 * Kernel::Memory::PageSize};
        const auto start_info{page_table.QueryInfo(start - 1)};

        if (start_info.state != Kernel::Memory::MemoryState::Free) {
            return {};
        }

        if (start_info.GetAddress() > (start - padding_size)) {
            return {};
        }

        const auto end_info{page_table.QueryInfo(start + size)};

        if (end_info.state != Kernel::Memory::MemoryState::Free) {
            return {};
        }

        return (start + size + padding_size) <= (end_info.GetAddress() + end_info.GetSize());
    }

    VAddr GetRandomMapRegion(const Kernel::Memory::PageTable& page_table, std::size_t size) const {
        VAddr addr{};
        const std::size_t end_pages{(page_table.GetAliasCodeRegionSize() - size) >>
                                    Kernel::Memory::PageBits};
        do {
            addr = page_table.GetAliasCodeRegionStart() +
                   (Kernel::Memory::SystemControl::GenerateRandomRange(0, end_pages)
                    << Kernel::Memory::PageBits);
        } while (!page_table.IsInsideAddressSpace(addr, size) ||
                 page_table.IsInsideHeapRegion(addr, size) ||
                 page_table.IsInsideAliasRegion(addr, size));
        return addr;
    }

    ResultVal<VAddr> MapProcessCodeMemory(Kernel::Process* process, VAddr baseAddress,
                                          u64 size) const {
        for (std::size_t retry = 0; retry < MAXIMUM_MAP_RETRIES; retry++) {
            auto& page_table{process->PageTable()};
            const VAddr addr{GetRandomMapRegion(page_table, size)};
            const ResultCode result{page_table.MapProcessCodeMemory(addr, baseAddress, size)};

            if (result == Kernel::ResultInvalidCurrentMemory) {
                continue;
            }

            CASCADE_CODE(result);

            if (ValidateRegionForMap(page_table, addr, size)) {
                return MakeResult<VAddr>(addr);
            }
        }

        return ERROR_INSUFFICIENT_ADDRESS_SPACE;
    }

    ResultVal<VAddr> MapNro(Kernel::Process* process, VAddr nro_addr, std::size_t nro_size,
                            VAddr bss_addr, std::size_t bss_size, std::size_t size) const {
        for (std::size_t retry = 0; retry < MAXIMUM_MAP_RETRIES; retry++) {
            auto& page_table{process->PageTable()};
            VAddr addr{};

            CASCADE_RESULT(addr, MapProcessCodeMemory(process, nro_addr, nro_size));

            if (bss_size) {
                auto block_guard = detail::ScopeExit([&] {
                    page_table.UnmapProcessCodeMemory(addr + nro_size, bss_addr, bss_size);
                    page_table.UnmapProcessCodeMemory(addr, nro_addr, nro_size);
                });

                const ResultCode result{
                    page_table.MapProcessCodeMemory(addr + nro_size, bss_addr, bss_size)};

                if (result == Kernel::ResultInvalidCurrentMemory) {
                    continue;
                }

                if (result.IsError()) {
                    return result;
                }

                block_guard.Cancel();
            }

            if (ValidateRegionForMap(page_table, addr, size)) {
                return MakeResult<VAddr>(addr);
            }
        }

        return ERROR_INSUFFICIENT_ADDRESS_SPACE;
    }

    ResultCode LoadNro(Kernel::Process* process, const NROHeader& nro_header, VAddr nro_addr,
                       VAddr start) const {
        const VAddr text_start{start + nro_header.segment_headers[TEXT_INDEX].memory_offset};
        const VAddr ro_start{start + nro_header.segment_headers[RO_INDEX].memory_offset};
        const VAddr data_start{start + nro_header.segment_headers[DATA_INDEX].memory_offset};
        const VAddr bss_start{data_start + nro_header.segment_headers[DATA_INDEX].memory_size};
        const VAddr bss_end_addr{
            Common::AlignUp(bss_start + nro_header.bss_size, Kernel::Memory::PageSize)};

        auto CopyCode{[&](VAddr src_addr, VAddr dst_addr, u64 size) {
            std::vector<u8> source_data(size);
            system.Memory().ReadBlock(src_addr, source_data.data(), source_data.size());
            system.Memory().WriteBlock(dst_addr, source_data.data(), source_data.size());
        }};
        CopyCode(nro_addr + nro_header.segment_headers[TEXT_INDEX].memory_offset, text_start,
                 nro_header.segment_headers[TEXT_INDEX].memory_size);
        CopyCode(nro_addr + nro_header.segment_headers[RO_INDEX].memory_offset, ro_start,
                 nro_header.segment_headers[RO_INDEX].memory_size);
        CopyCode(nro_addr + nro_header.segment_headers[DATA_INDEX].memory_offset, data_start,
                 nro_header.segment_headers[DATA_INDEX].memory_size);

        CASCADE_CODE(process->PageTable().SetCodeMemoryPermission(
            text_start, ro_start - text_start, Kernel::Memory::MemoryPermission::ReadAndExecute));
        CASCADE_CODE(process->PageTable().SetCodeMemoryPermission(
            ro_start, data_start - ro_start, Kernel::Memory::MemoryPermission::Read));

        return process->PageTable().SetCodeMemoryPermission(
            data_start, bss_end_addr - data_start, Kernel::Memory::MemoryPermission::ReadAndWrite);
    }

    void LoadNro(Kernel::HLERequestContext& ctx) {
        struct Parameters {
            u64_le process_id;
            u64_le image_address;
            u64_le image_size;
            u64_le bss_address;
            u64_le bss_size;
        };

        IPC::RequestParser rp{ctx};
        const auto [process_id, nro_address, nro_size, bss_address, bss_size] =
            rp.PopRaw<Parameters>();

        LOG_DEBUG(Service_LDR,
                  "called with pid={:016X}, nro_addr={:016X}, nro_size={:016X}, bss_addr={:016X}, "
                  "bss_size={:016X}",
                  process_id, nro_address, nro_size, bss_address, bss_size);

        if (!initialized) {
            LOG_ERROR(Service_LDR, "LDR:RO not initialized before use!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_NOT_INITIALIZED);
            return;
        }

        if (nro.size() >= MAXIMUM_LOADED_RO) {
            LOG_ERROR(Service_LDR, "Loading new NRO would exceed the maximum number of loaded NROs "
                                   "(0x40)! Failing...");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_MAXIMUM_NRO);
            return;
        }

        // NRO Address does not fall on 0x1000 byte boundary
        if (!Common::Is4KBAligned(nro_address)) {
            LOG_ERROR(Service_LDR, "NRO Address has invalid alignment (actual {:016X})!",
                      nro_address);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_INVALID_ALIGNMENT);
            return;
        }

        // NRO Size or BSS Size is zero or causes overflow
        const auto nro_size_valid =
            nro_size != 0 && nro_address + nro_size > nro_address && Common::Is4KBAligned(nro_size);
        const auto bss_size_valid = nro_size + bss_size >= nro_size &&
                                    (bss_size == 0 || bss_address + bss_size > bss_address);

        if (!nro_size_valid || !bss_size_valid) {
            LOG_ERROR(Service_LDR,
                      "NRO Size or BSS Size is invalid! (nro_address={:016X}, nro_size={:016X}, "
                      "bss_address={:016X}, bss_size={:016X})",
                      nro_address, nro_size, bss_address, bss_size);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_INVALID_SIZE);
            return;
        }

        // Read NRO data from memory
        std::vector<u8> nro_data(nro_size);
        system.Memory().ReadBlock(nro_address, nro_data.data(), nro_size);

        SHA256Hash hash{};
        mbedtls_sha256_ret(nro_data.data(), nro_data.size(), hash.data(), 0);

        // NRO Hash is already loaded
        if (std::any_of(nro.begin(), nro.end(), [&hash](const std::pair<VAddr, NROInfo>& info) {
                return info.second.hash == hash;
            })) {
            LOG_ERROR(Service_LDR, "NRO is already loaded!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_ALREADY_LOADED);
            return;
        }

        // NRO Hash is not in any loaded NRR
        if (!IsValidNROHash(hash)) {
            LOG_ERROR(Service_LDR,
                      "NRO hash is not present in any currently loaded NRRs (hash={})!",
                      Common::HexToString(hash));
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_MISSING_NRR_HASH);
            return;
        }

        // Load and validate the NRO header
        NROHeader header{};
        std::memcpy(&header, nro_data.data(), sizeof(NROHeader));
        if (!IsValidNRO(header, nro_size, bss_size)) {
            LOG_ERROR(Service_LDR, "NRO was invalid!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_INVALID_NRO);
            return;
        }

        // Map memory for the NRO
        const auto map_result{MapNro(system.CurrentProcess(), nro_address, nro_size, bss_address,
                                     bss_size, nro_size + bss_size)};
        if (map_result.Failed()) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(map_result.Code());
        }

        // Load the NRO into the mapped memory
        if (const auto result{LoadNro(system.CurrentProcess(), header, nro_address, *map_result)};
            result.IsError()) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(map_result.Code());
        }

        // Track the loaded NRO
        nro.insert_or_assign(*map_result,
                             NROInfo{hash, *map_result, nro_size, bss_address, bss_size,
                                     header.segment_headers[TEXT_INDEX].memory_size,
                                     header.segment_headers[RO_INDEX].memory_size,
                                     header.segment_headers[DATA_INDEX].memory_size, nro_address});

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(RESULT_SUCCESS);
        rb.Push(*map_result);
    }

    ResultCode UnmapNro(const NROInfo& info) {
        // Each region must be unmapped separately to validate memory state
        auto& page_table{system.CurrentProcess()->PageTable()};
        CASCADE_CODE(page_table.UnmapProcessCodeMemory(info.nro_address + info.text_size +
                                                           info.ro_size + info.data_size,
                                                       info.bss_address, info.bss_size));
        CASCADE_CODE(page_table.UnmapProcessCodeMemory(
            info.nro_address + info.text_size + info.ro_size,
            info.src_addr + info.text_size + info.ro_size, info.data_size));
        CASCADE_CODE(page_table.UnmapProcessCodeMemory(
            info.nro_address + info.text_size, info.src_addr + info.text_size, info.ro_size));
        CASCADE_CODE(
            page_table.UnmapProcessCodeMemory(info.nro_address, info.src_addr, info.text_size));
        return RESULT_SUCCESS;
    }

    void UnloadNro(Kernel::HLERequestContext& ctx) {
        if (!initialized) {
            LOG_ERROR(Service_LDR, "LDR:RO not initialized before use!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_NOT_INITIALIZED);
            return;
        }

        struct Parameters {
            u64_le process_id;
            u64_le nro_address;
        };

        IPC::RequestParser rp{ctx};
        const auto [process_id, nro_address] = rp.PopRaw<Parameters>();
        LOG_DEBUG(Service_LDR, "called with process_id={:016X}, nro_address=0x{:016X}", process_id,
                  nro_address);

        if (!Common::Is4KBAligned(nro_address)) {
            LOG_ERROR(Service_LDR, "NRO address has invalid alignment (nro_address=0x{:016X})",
                      nro_address);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_INVALID_ALIGNMENT);
            return;
        }

        const auto iter = nro.find(nro_address);
        if (iter == nro.end()) {
            LOG_ERROR(Service_LDR,
                      "The NRO attempting to be unmapped was not mapped or has an invalid address "
                      "(nro_address=0x{:016X})!",
                      nro_address);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ERROR_INVALID_NRO_ADDRESS);
            return;
        }

        const auto result{UnmapNro(iter->second)};

        nro.erase(iter);

        IPC::ResponseBuilder rb{ctx, 2};

        rb.Push(result);
    }

    void Initialize(Kernel::HLERequestContext& ctx) {
        LOG_WARNING(Service_LDR, "(STUBBED) called");

        initialized = true;

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(RESULT_SUCCESS);
    }

private:
    bool initialized{};

    std::map<VAddr, NROInfo> nro;
    std::map<VAddr, std::vector<SHA256Hash>> nrr;

    bool IsValidNROHash(const SHA256Hash& hash) const {
        return std::any_of(nrr.begin(), nrr.end(), [&hash](const auto& p) {
            return std::find(p.second.begin(), p.second.end(), hash) != p.second.end();
        });
    }

    static bool IsValidNRO(const NROHeader& header, u64 nro_size, u64 bss_size) {
        return header.magic == Common::MakeMagic('N', 'R', 'O', '0') &&
               header.nro_size == nro_size && header.bss_size == bss_size &&

               header.segment_headers[RO_INDEX].memory_offset ==
                   header.segment_headers[TEXT_INDEX].memory_offset +
                       header.segment_headers[TEXT_INDEX].memory_size &&

               header.segment_headers[DATA_INDEX].memory_offset ==
                   header.segment_headers[RO_INDEX].memory_offset +
                       header.segment_headers[RO_INDEX].memory_size &&

               nro_size == header.segment_headers[DATA_INDEX].memory_offset +
                               header.segment_headers[DATA_INDEX].memory_size &&

               Common::Is4KBAligned(header.segment_headers[TEXT_INDEX].memory_size) &&
               Common::Is4KBAligned(header.segment_headers[RO_INDEX].memory_size) &&
               Common::Is4KBAligned(header.segment_headers[DATA_INDEX].memory_size);
    }
};

void InstallInterfaces(SM::ServiceManager& sm, Core::System& system) {
    std::make_shared<DebugMonitor>(system)->InstallAsService(sm);
    std::make_shared<ProcessManager>(system)->InstallAsService(sm);
    std::make_shared<Shell>(system)->InstallAsService(sm);
    std::make_shared<RelocatableObject>(system)->InstallAsService(sm);
}

} // namespace Service::LDR
