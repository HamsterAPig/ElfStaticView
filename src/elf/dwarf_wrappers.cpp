#include "elf/dwarf_wrappers.hpp"

#include "elf/elf_symbol_table.hpp"
#include "elf/ti_coff_object.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace elf_static_view::elf {

namespace {

    [[nodiscard]] std::string build_error_message(const std::string& prefix, Dwarf_Error error)
    {
        std::ostringstream stream;
        stream << prefix;
        if (error != nullptr) {
            stream << ": " << dwarf_errmsg(error);
        }
        return stream.str();
    }

    [[nodiscard]] std::optional<Dwarf_Unsigned> read_manual_ref_sup_offset(Dwarf_Attribute attr, const Dwarf_Half form)
    {
        if (form != DW_FORM_ref_sup4 && form != DW_FORM_ref_sup8) {
            return std::nullopt;
        }

        struct ManualAttributeView {
            Dwarf_Half attribute = 0;
            Dwarf_Half form = 0;
            Dwarf_Half direct_form = 0;
            void* cu_context = nullptr;
            Dwarf_Small* debug_ptr = nullptr;
            Dwarf_Signed implicit_const = 0;
            Dwarf_Debug debug = nullptr;
        };

        const auto* raw_attr = reinterpret_cast<const ManualAttributeView*>(attr);
        if (raw_attr == nullptr || raw_attr->debug_ptr == nullptr) {
            return std::nullopt;
        }

        Dwarf_Unsigned offset = 0;
        const std::size_t width = form == DW_FORM_ref_sup8 ? 8U : 4U;
        // 当前 supplementary 引用样本来自 x86_64 little-endian GCC fixture；这里按固定宽度小端读取 payload。
        for (std::size_t index = 0; index < width; ++index) {
            offset |= static_cast<Dwarf_Unsigned>(static_cast<unsigned char>(raw_attr->debug_ptr[index]))
                      << (index * 8U);
        }
        return offset;
    }

    [[nodiscard]] std::string sig8_to_hex(const Dwarf_Sig8& signature)
    {
        std::ostringstream stream;
        stream << "0x";
        stream << std::hex << std::setfill('0');
        for (unsigned char byte : signature.signature) {
            stream << std::setw(2) << static_cast<unsigned int>(byte);
        }
        return stream.str();
    }

    [[nodiscard]] bool should_trace_sig8(const Dwarf_Sig8& signature)
    {
        const char* raw = std::getenv("ELF_STATIC_VIEW_TRACE_SIG8");
        if (raw == nullptr || *raw == '\0') {
            return false;
        }
        const std::string filter = raw;
        if (filter == "1" || filter == "all" || filter == "*") {
            return true;
        }
        const auto hex = sig8_to_hex(signature);
        return hex == filter || hex.substr(2) == filter;
    }

    [[nodiscard]] const char* fallback_location_op_name(const Dwarf_Small atom)
    {
        switch (atom) {
            case DW_OP_addr:
                return "DW_OP_addr";
            case DW_OP_deref:
                return "DW_OP_deref";
            case DW_OP_constu:
                return "DW_OP_constu";
            case DW_OP_consts:
                return "DW_OP_consts";
            case DW_OP_dup:
                return "DW_OP_dup";
            case DW_OP_drop:
                return "DW_OP_drop";
            case DW_OP_over:
                return "DW_OP_over";
            case DW_OP_swap:
                return "DW_OP_swap";
            case DW_OP_rot:
                return "DW_OP_rot";
            case DW_OP_xderef:
                return "DW_OP_xderef";
            case DW_OP_abs:
                return "DW_OP_abs";
            case DW_OP_and:
                return "DW_OP_and";
            case DW_OP_div:
                return "DW_OP_div";
            case DW_OP_minus:
                return "DW_OP_minus";
            case DW_OP_mod:
                return "DW_OP_mod";
            case DW_OP_mul:
                return "DW_OP_mul";
            case DW_OP_neg:
                return "DW_OP_neg";
            case DW_OP_not:
                return "DW_OP_not";
            case DW_OP_or:
                return "DW_OP_or";
            case DW_OP_plus:
                return "DW_OP_plus";
            case DW_OP_plus_uconst:
                return "DW_OP_plus_uconst";
            case DW_OP_shl:
                return "DW_OP_shl";
            case DW_OP_shr:
                return "DW_OP_shr";
            case DW_OP_shra:
                return "DW_OP_shra";
            case DW_OP_xor:
                return "DW_OP_xor";
            case DW_OP_bra:
                return "DW_OP_bra";
            case DW_OP_eq:
                return "DW_OP_eq";
            case DW_OP_ge:
                return "DW_OP_ge";
            case DW_OP_gt:
                return "DW_OP_gt";
            case DW_OP_le:
                return "DW_OP_le";
            case DW_OP_lt:
                return "DW_OP_lt";
            case DW_OP_ne:
                return "DW_OP_ne";
            case DW_OP_skip:
                return "DW_OP_skip";
            case DW_OP_fbreg:
                return "DW_OP_fbreg";
            case DW_OP_piece:
                return "DW_OP_piece";
            case DW_OP_deref_size:
                return "DW_OP_deref_size";
            case DW_OP_xderef_size:
                return "DW_OP_xderef_size";
            case DW_OP_nop:
                return "DW_OP_nop";
            case DW_OP_call_frame_cfa:
                return "DW_OP_call_frame_cfa";
            case DW_OP_bit_piece:
                return "DW_OP_bit_piece";
            case DW_OP_implicit_value:
                return "DW_OP_implicit_value";
            case DW_OP_stack_value:
                return "DW_OP_stack_value";
            case DW_OP_addrx:
                return "DW_OP_addrx";
            case DW_OP_constx:
                return "DW_OP_constx";
            case DW_OP_entry_value:
                return "DW_OP_entry_value";
            case DW_OP_const_type:
                return "DW_OP_const_type";
            case DW_OP_regval_type:
                return "DW_OP_regval_type";
            case DW_OP_deref_type:
                return "DW_OP_deref_type";
            case DW_OP_xderef_type:
                return "DW_OP_xderef_type";
            case DW_OP_convert:
                return "DW_OP_convert";
            case DW_OP_reinterpret:
                return "DW_OP_reinterpret";
            case DW_OP_GNU_addr_index:
                return "DW_OP_GNU_addr_index";
            default:
                break;
        }

        if (atom >= DW_OP_lit0 && atom <= DW_OP_lit31) {
            return "DW_OP_lit";
        }
        if (atom >= DW_OP_reg0 && atom <= DW_OP_reg31) {
            return "DW_OP_reg";
        }
        if (atom >= DW_OP_breg0 && atom <= DW_OP_breg31) {
            return "DW_OP_breg";
        }
        return nullptr;
    }

    void assign_location_op_name(LocationOp& op)
    {
        const char* op_name = nullptr;
        if (dwarf_get_OP_name(op.atom, &op_name) == DW_DLV_OK && op_name != nullptr) {
            op.name = op_name;
            return;
        }

        const char* fallback = fallback_location_op_name(op.atom);
        if (fallback == nullptr) {
            return;
        }

        if (std::strcmp(fallback, "DW_OP_lit") == 0) {
            op.name = "DW_OP_lit" + std::to_string(static_cast<unsigned int>(op.atom - DW_OP_lit0));
            return;
        }
        if (std::strcmp(fallback, "DW_OP_reg") == 0) {
            op.name = "DW_OP_reg" + std::to_string(static_cast<unsigned int>(op.atom - DW_OP_reg0));
            return;
        }
        if (std::strcmp(fallback, "DW_OP_breg") == 0) {
            op.name = "DW_OP_breg" + std::to_string(static_cast<unsigned int>(op.atom - DW_OP_breg0));
            return;
        }

        op.name = fallback;
    }

    void trace_sig8(const Dwarf_Sig8& signature, const std::string& message)
    {
        if (!should_trace_sig8(signature)) {
            return;
        }
        std::cerr << "[trace-sig8] " << sig8_to_hex(signature) << " | " << message << '\n';
    }

    void destroy_error(Dwarf_Error error)
    {
        if (error != nullptr) {
            dwarf_dealloc_error(nullptr, error);
        }
    }

    [[nodiscard]] std::optional<std::filesystem::path> build_split_dwarf_sidecar_path(
        const std::filesystem::path& file_path)
    {
        const auto filename = file_path.filename().string();
        const auto sibling = file_path.parent_path() / (filename + "-" + file_path.stem().string() + ".dwo");
        if (std::filesystem::exists(sibling)) {
            return sibling;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::filesystem::path> build_debug_sup_sidecar_path(
        const std::filesystem::path& file_path, const std::string& filename_from_debug_sup)
    {
        if (filename_from_debug_sup.empty()) {
            return std::nullopt;
        }
        const auto sibling = file_path.parent_path() / std::filesystem::path(filename_from_debug_sup);
        if (std::filesystem::exists(sibling)) {
            return sibling;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::filesystem::path> build_gnu_debugaltlink_sidecar_path(
        const std::filesystem::path& file_path, const std::string& filename_from_altlink)
    {
        if (filename_from_altlink.empty()) {
            return std::nullopt;
        }
        const auto sibling = file_path.parent_path() / std::filesystem::path(filename_from_altlink);
        if (std::filesystem::exists(sibling)) {
            return sibling;
        }
        return std::nullopt;
    }

    using Sig8Map = std::unordered_map<std::string, Dwarf_Off>;

    [[nodiscard]] std::uint32_t read_u32_le(const std::vector<std::uint8_t>& data, const std::size_t offset)
    {
        if (offset + 4 > data.size()) {
            throw DwarfError("读取 .debug_types 失败: 越界读取 u32");
        }
        return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
               (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
               (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
    }

    [[nodiscard]] std::uint16_t read_u16_le(const std::vector<std::uint8_t>& data, const std::size_t offset)
    {
        if (offset + 2 > data.size()) {
            throw DwarfError("读取 .debug_types 失败: 越界读取 u16");
        }
        return static_cast<std::uint16_t>(data[offset]) | (static_cast<std::uint16_t>(data[offset + 1]) << 8U);
    }

    [[nodiscard]] Sig8Map& debug_types_sig8_map(const std::string& file_path)
    {
        static std::unordered_map<std::string, Sig8Map> cache;
        const auto found = cache.find(file_path);
        if (found != cache.end()) {
            return found->second;
        }

        Sig8Map map;
        const auto section = ElfSymbolTable::read_section_bytes(file_path, ".debug_types");
        if (section.has_value()) {
            const auto& data = section.value();
            std::size_t offset = 0;
            while (offset + 23 <= data.size()) {
                const auto unit_length = read_u32_le(data, offset);
                if (unit_length == 0) {
                    offset += 4;
                    continue;
                }
                const std::size_t unit_end = offset + 4 + unit_length;
                if (unit_end > data.size()) {
                    break;
                }
                const auto version = read_u16_le(data, offset + 4);
                if (version == 4) {
                    Dwarf_Sig8 signature{};
                    // DWARF4 type unit 头布局：
                    // unit_length(4) + version(2) + abbrev_offset(4) + address_size(1)
                    // + type_signature(8) + type_offset(4)
                    std::memcpy(signature.signature, data.data() + offset + 11, sizeof(signature.signature));
                    const auto type_offset = read_u32_le(data, offset + 19);
                    if (type_offset < unit_length + 4) {
                        map[sig8_to_hex(signature)] = static_cast<Dwarf_Off>(offset + type_offset);
                    }
                }
                offset = unit_end;
            }
        }

        return cache.emplace(file_path, std::move(map)).first->second;
    }

} // namespace

DwarfError::DwarfError(const std::string& message) : std::runtime_error(message) {}

Dwarf_Debug DebugHandle::open_debug(const std::string& file_path,
                                    const ObjectFileKind file_kind,
                                    const unsigned int group_number,
                                    const char* prefix)
{
    if (file_kind != ObjectFileKind::Elf) {
        if (file_kind == ObjectFileKind::TiCoff) {
            throw DwarfError("TI-COFF 对象需要通过 dwarf_object_init_b 打开");
        }
        throw DwarfError("不支持的对象格式: 仅支持 ELF/EABI 与 TI C2000 COFF v2 executable .out");
    }
    Dwarf_Error error = nullptr;
    char actual_path_buffer[2048] = {};
    Dwarf_Debug debug = nullptr;
    const int result = dwarf_init_path(file_path.c_str(),
                                       actual_path_buffer,
                                       sizeof(actual_path_buffer),
                                       group_number,
                                       nullptr,
                                       nullptr,
                                       &debug,
                                       &error);
    if (result != DW_DLV_OK) {
        const auto message = build_error_message(prefix, error);
        destroy_error(error);
        throw DwarfError(message);
    }
    return debug;
}

std::optional<std::filesystem::path> DebugHandle::detect_split_dwarf_path(const std::string& file_path)
{
    const std::filesystem::path path(file_path);
    if (const auto sibling = build_split_dwarf_sidecar_path(path); sibling.has_value()) {
        return sibling;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> DebugHandle::detect_debug_sup_path(const std::string& file_path)
{
    Dwarf_Debug primary_debug = nullptr;
    try {
        primary_debug =
            open_debug(file_path, ObjectFileKind::Elf, DW_GROUPNUMBER_ANY, "dwarf_init_path(primary) failed");

        Dwarf_Half version = 0;
        Dwarf_Small is_supplementary = 0;
        char* filename = nullptr;
        Dwarf_Unsigned checksum_len = 0;
        Dwarf_Small* checksum = nullptr;
        Dwarf_Error error = nullptr;
        const int result = dwarf_get_debug_sup(
            primary_debug, &version, &is_supplementary, &filename, &checksum_len, &checksum, &error);
        if (result == DW_DLV_NO_ENTRY) {
            dwarf_finish(primary_debug);
            return std::nullopt;
        }
        if (result != DW_DLV_OK) {
            const auto message = build_error_message("dwarf_get_debug_sup failed", error);
            destroy_error(error);
            dwarf_finish(primary_debug);
            throw DwarfError(message);
        }

        // 现有样本里 .debug_sup 的 is_supplementary 同时出现过 0 和 1。
        // 对当前加载器来说，只要这是合法的 .debug_sup 且带有可解析的 sibling 文件名，
        // 就应尝试自动绑定 tied debug，避免把 supplementary string/ref form 卡死在样本差异上。
        if (version != 2 || is_supplementary > 1 || filename == nullptr || *filename == '\0') {
            dwarf_finish(primary_debug);
            return std::nullopt;
        }

        const auto sidecar_path = build_debug_sup_sidecar_path(std::filesystem::path(file_path), std::string(filename));
        dwarf_finish(primary_debug);
        return sidecar_path;
    } catch (...) {
        if (primary_debug != nullptr) {
            dwarf_finish(primary_debug);
        }
        throw;
    }
}

std::optional<std::filesystem::path> DebugHandle::detect_gnu_debugaltlink_path(const std::string& file_path)
{
    const auto section = ElfSymbolTable::read_section_bytes(file_path, ".gnu_debugaltlink");
    if (!section.has_value() || section->empty()) {
        return std::nullopt;
    }

    const auto* raw = reinterpret_cast<const char*>(section->data());
    const auto size = section->size();
    std::size_t filename_len = 0;
    while (filename_len < size && raw[filename_len] != '\0') {
        ++filename_len;
    }
    if (filename_len == 0 || filename_len >= size) {
        return std::nullopt;
    }

    const std::string alt_filename(raw, filename_len);
    return build_gnu_debugaltlink_sidecar_path(std::filesystem::path(file_path), alt_filename);
}

void DebugHandle::tie_debug_handles(Dwarf_Debug primary_debug, Dwarf_Debug secondary_debug, const char* error_prefix)
{
    Dwarf_Error error = nullptr;
    const int tie_result = dwarf_set_tied_dbg(primary_debug, secondary_debug, &error);
    if (tie_result == DW_DLV_OK) {
        return;
    }
    const auto message = build_error_message(error_prefix, error);
    destroy_error(error);
    throw DwarfError(message);
}

DebugHandle::DebugHandle(const std::string& file_path) : DebugHandle(file_path, detect_object_file_kind(file_path)) {}

DebugHandle::DebugHandle(const std::string& file_path, const ObjectFileKind file_kind) : file_path_(file_path)
{
    if (file_kind != ObjectFileKind::Elf) {
        if (file_kind != ObjectFileKind::TiCoff) {
            throw DwarfError("不支持的对象格式: 仅支持 ELF/EABI 与 TI C2000 COFF v2 executable .out");
        }

        // TI-COFF 不走 libdwarf 内置 dwarf_init_path 探测；这里用自研 section reader
        // 组装 object access，让后续 CU/DIE/变量解析继续复用同一套 Dwarf_Debug。
        ti_coff_object_ = std::make_unique<TiCoffObject>(file_path);
        const auto missing_sections = ti_coff_object_->missing_required_debug_sections();
        if (!missing_sections.empty()) {
            std::ostringstream message;
            message << "TI-COFF 缺少必要 DWARF section";
            for (const auto& name : missing_sections) {
                message << ' ' << name;
            }
            throw DwarfError(message.str());
        }

        ti_coff_access_ = make_ti_coff_dwarf_access(*ti_coff_object_);
        Dwarf_Error error = nullptr;
        const int result = dwarf_object_init_b(&ti_coff_access_, nullptr, nullptr, DW_GROUPNUMBER_ANY, &debug_, &error);
        if (result != DW_DLV_OK) {
            const auto message = build_error_message("dwarf_object_init_b(TI-COFF) failed", error);
            destroy_error(error);
            throw DwarfError(message);
        }
        backend_ = Backend::TiCoff;
        return;
    }

    // 当前已覆盖三类自动接线：
    // 1. split DWARF skeleton + sidecar .dwo
    // 2. 主对象里的 .debug_sup -> 同目录 supplementary object
    // 3. 主对象里的 .gnu_debugaltlink -> 同目录 alternate object
    if (const auto split_dwarf_path = detect_split_dwarf_path(file_path); split_dwarf_path.has_value()) {
        tied_debug_ = open_debug(file_path, file_kind, DW_GROUPNUMBER_BASE, "dwarf_init_path(base) failed");
        debug_ = open_debug(
            split_dwarf_path->string(), ObjectFileKind::Elf, DW_GROUPNUMBER_DWO, "dwarf_init_path(dwo) failed");
        try {
            tie_debug_handles(debug_, tied_debug_, "dwarf_set_tied_dbg(dwo) failed");
            return;
        } catch (...) {
            if (debug_ != nullptr) {
                dwarf_finish(debug_);
                debug_ = nullptr;
            }
            if (tied_debug_ != nullptr) {
                dwarf_finish(tied_debug_);
                tied_debug_ = nullptr;
            }
            throw;
        }
    }

    debug_ = open_debug(file_path, file_kind, DW_GROUPNUMBER_ANY, "dwarf_init_path failed");
    if (const auto debug_alt_path = detect_gnu_debugaltlink_path(file_path); debug_alt_path.has_value()) {
        try {
            tied_debug_ = open_debug(debug_alt_path->string(),
                                     ObjectFileKind::Elf,
                                     DW_GROUPNUMBER_ANY,
                                     "dwarf_init_path(gnu_debugaltlink sidecar) failed");
            tie_debug_handles(debug_, tied_debug_, "dwarf_set_tied_dbg(gnu_debugaltlink) failed");
            return;
        } catch (...) {
            if (tied_debug_ != nullptr) {
                dwarf_finish(tied_debug_);
                tied_debug_ = nullptr;
            }
            throw;
        }
    }
    if (const auto debug_sup_path = detect_debug_sup_path(file_path); debug_sup_path.has_value()) {
        try {
            tied_debug_ = open_debug(debug_sup_path->string(),
                                     ObjectFileKind::Elf,
                                     DW_GROUPNUMBER_ANY,
                                     "dwarf_init_path(debug_sup sidecar) failed");
            tie_debug_handles(debug_, tied_debug_, "dwarf_set_tied_dbg(debug_sup) failed");
        } catch (...) {
            if (tied_debug_ != nullptr) {
                dwarf_finish(tied_debug_);
                tied_debug_ = nullptr;
            }
            if (debug_ != nullptr) {
                dwarf_finish(debug_);
                debug_ = nullptr;
            }
            throw;
        }
    }
}

DebugHandle::~DebugHandle()
{
    if (debug_ != nullptr) {
        if (backend_ == Backend::TiCoff) {
            dwarf_object_finish(debug_);
        } else {
            dwarf_finish(debug_);
        }
    }
    if (tied_debug_ != nullptr) {
        dwarf_finish(tied_debug_);
    }
}

Dwarf_Debug DebugHandle::get() const noexcept
{
    return debug_;
}

const std::string& DebugHandle::file_path() const noexcept
{
    return file_path_;
}

DieHandle::DieHandle(Dwarf_Debug debug, Dwarf_Die die) : debug_(debug), die_(die) {}

DieHandle::~DieHandle()
{
    reset();
}

DieHandle::DieHandle(DieHandle&& other) noexcept : debug_(other.debug_), die_(other.die_)
{
    other.debug_ = nullptr;
    other.die_ = nullptr;
}

DieHandle& DieHandle::operator=(DieHandle&& other) noexcept
{
    if (this != &other) {
        reset();
        debug_ = other.debug_;
        die_ = other.die_;
        other.debug_ = nullptr;
        other.die_ = nullptr;
    }
    return *this;
}

Dwarf_Die DieHandle::get() const noexcept
{
    return die_;
}

DieHandle::operator bool() const noexcept
{
    return die_ != nullptr;
}

void DieHandle::reset() noexcept
{
    if (debug_ != nullptr && die_ != nullptr) {
        dwarf_dealloc_die(die_);
    }
    debug_ = nullptr;
    die_ = nullptr;
}

AttributeHandle::AttributeHandle(Dwarf_Debug debug, Dwarf_Attribute attribute) : debug_(debug), attribute_(attribute) {}

AttributeHandle::~AttributeHandle()
{
    reset();
}

AttributeHandle::AttributeHandle(AttributeHandle&& other) noexcept : debug_(other.debug_), attribute_(other.attribute_)
{
    other.debug_ = nullptr;
    other.attribute_ = nullptr;
}

AttributeHandle& AttributeHandle::operator=(AttributeHandle&& other) noexcept
{
    if (this != &other) {
        reset();
        debug_ = other.debug_;
        attribute_ = other.attribute_;
        other.debug_ = nullptr;
        other.attribute_ = nullptr;
    }
    return *this;
}

Dwarf_Attribute AttributeHandle::get() const noexcept
{
    return attribute_;
}

AttributeHandle::operator bool() const noexcept
{
    return attribute_ != nullptr;
}

void AttributeHandle::reset() noexcept
{
    if (debug_ != nullptr && attribute_ != nullptr) {
        dwarf_dealloc_attribute(attribute_);
    }
    debug_ = nullptr;
    attribute_ = nullptr;
}

std::optional<DieHandle> child_of(Dwarf_Debug debug, Dwarf_Die die)
{
    Dwarf_Die child = nullptr;
    Dwarf_Error error = nullptr;
    const int result = dwarf_child(die, &child, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        const auto message = build_error_message("dwarf_child failed", error);
        destroy_error(error);
        throw DwarfError(message);
    }
    return DieHandle(debug, child);
}

std::optional<DieHandle> sibling_of(Dwarf_Debug debug, Dwarf_Die die, bool is_info)
{
    Dwarf_Die sibling = nullptr;
    Dwarf_Error error = nullptr;
    const int result = dwarf_siblingof_b(debug, die, is_info, &sibling, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        const auto message = build_error_message("dwarf_siblingof_b failed", error);
        destroy_error(error);
        throw DwarfError(message);
    }
    return DieHandle(debug, sibling);
}

std::optional<AttributeHandle> attribute_of(Dwarf_Debug debug, Dwarf_Die die, Dwarf_Half attr)
{
    Dwarf_Attribute attribute = nullptr;
    Dwarf_Error error = nullptr;
    const int result = dwarf_attr(die, attr, &attribute, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        const auto message = build_error_message("dwarf_attr failed", error);
        destroy_error(error);
        throw DwarfError(message);
    }
    return AttributeHandle(debug, attribute);
}

std::optional<std::string> die_name(Dwarf_Die die)
{
    char* name = nullptr;
    Dwarf_Error error = nullptr;
    const int result = dwarf_diename(die, &name, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        const auto message = build_error_message("dwarf_diename failed", error);
        destroy_error(error);
        throw DwarfError(message);
    }
    return std::string(name);
}

Dwarf_Half die_tag(Dwarf_Die die)
{
    Dwarf_Half tag = 0;
    Dwarf_Error error = nullptr;
    const int result = dwarf_tag(die, &tag, &error);
    if (result != DW_DLV_OK) {
        const auto message = build_error_message("dwarf_tag failed", error);
        destroy_error(error);
        throw DwarfError(message);
    }
    return tag;
}

std::optional<Dwarf_Off> die_offset(Dwarf_Die die)
{
    Dwarf_Off offset = 0;
    Dwarf_Error error = nullptr;
    const int result = dwarf_dieoffset(die, &offset, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        const auto message = build_error_message("dwarf_dieoffset failed", error);
        destroy_error(error);
        throw DwarfError(message);
    }
    return offset;
}

std::optional<Dwarf_Off> global_type_offset(Dwarf_Attribute attr)
{
    Dwarf_Half form = 0;
    Dwarf_Error form_error = nullptr;
    const int form_result = dwarf_whatform(attr, &form, &form_error);
    if (form_result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (form_result != DW_DLV_OK) {
        const auto message = build_error_message("dwarf_whatform failed", form_error);
        destroy_error(form_error);
        throw DwarfError(message);
    }
    if (form == DW_FORM_ref_sup4 || form == DW_FORM_ref_sup8) {
        const auto offset = read_manual_ref_sup_offset(attr, form);
        if (!offset.has_value()) {
            return std::nullopt;
        }
        return static_cast<Dwarf_Off>(offset.value());
    }

    Dwarf_Off offset = 0;
    Dwarf_Bool is_info = true;
    Dwarf_Error error = nullptr;
    const int result = dwarf_global_formref_b(attr, &offset, &is_info, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        const auto message = build_error_message("dwarf_global_formref_b failed", error);
        destroy_error(error);
        throw DwarfError(message);
    }
    return offset;
}

std::optional<ReferenceTarget> type_reference_target(Dwarf_Debug debug,
                                                     Dwarf_Attribute attr,
                                                     const std::string* debug_file_path)
{
    Dwarf_Half form = 0;
    Dwarf_Error error = nullptr;
    const int form_result = dwarf_whatform(attr, &form, &error);
    if (form_result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (form_result != DW_DLV_OK) {
        const auto message = build_error_message("dwarf_whatform failed", error);
        destroy_error(error);
        throw DwarfError(message);
    }

    if (form == DW_FORM_ref_sup4 || form == DW_FORM_ref_sup8) {
        // libdwarf 当前不会替我们解析 ref_sup4/ref_sup8；这里直接读取 supplementary DIE 的全局偏移。
        const auto offset = read_manual_ref_sup_offset(attr, form);
        if (!offset.has_value()) {
            return std::nullopt;
        }
        return ReferenceTarget{.offset = static_cast<Dwarf_Off>(offset.value()), .is_info = true};
    }

    if (form == DW_FORM_ref_sig8) {
        Dwarf_Sig8 signature{};
        const int signature_result = dwarf_formsig8(attr, &signature, &error);
        if (signature_result == DW_DLV_NO_ENTRY) {
            return std::nullopt;
        }
        if (signature_result != DW_DLV_OK) {
            const auto message = build_error_message("dwarf_formsig8 failed", error);
            destroy_error(error);
            throw DwarfError(message);
        }

        Dwarf_Die referenced_die = nullptr;
        Dwarf_Bool is_info = true;
        error = nullptr;
        trace_sig8(signature, "enter ref_sig8 branch");
        const int find_result = dwarf_find_die_given_sig8(debug, &signature, &referenced_die, &is_info, &error);
        if (find_result == DW_DLV_NO_ENTRY) {
            return std::nullopt;
        }
        if (find_result != DW_DLV_OK) {
            trace_sig8(signature,
                       std::string("dwarf_find_die_given_sig8 failed: ") +
                           (error != nullptr ? dwarf_errmsg(error) : "<no-error>"));
            // TI C2000 的 ref_sig8 有时找不到目标 DIE，但能先找到对应的 type unit。
            // 这里先用 hash signature 找到 TU DIE，再取其第一个子 DIE 作为真实类型节点。
            destroy_error(error);
            referenced_die = nullptr;
            error = nullptr;
            const int tu_result = dwarf_die_from_hash_signature(debug, &signature, "tu", &referenced_die, &error);
            if (tu_result == DW_DLV_OK) {
                DieHandle tu_handle(debug, referenced_die);
                if (const auto tu_offset = die_offset(tu_handle.get()); tu_offset.has_value()) {
                    trace_sig8(signature, "tu die offset=" + std::to_string(tu_offset.value()));
                }
                Dwarf_Off type_offset = 0;
                Dwarf_Bool type_is_info = true;
                error = nullptr;
                const int type_offset_result =
                    dwarf_dietype_offset(tu_handle.get(), &type_offset, &type_is_info, &error);
                if (type_offset_result == DW_DLV_OK) {
                    trace_sig8(signature,
                               "dwarf_dietype_offset ok: offset=" + std::to_string(type_offset) +
                                   " is_info=" + std::to_string(type_is_info != 0));
                    return ReferenceTarget{.offset = type_offset, .is_info = type_is_info != 0};
                }
                trace_sig8(signature,
                           std::string("dwarf_dietype_offset failed: ") +
                               (error != nullptr ? dwarf_errmsg(error) : "<no-error>"));
                destroy_error(error);
                return std::nullopt;
            }
            trace_sig8(signature,
                       std::string("dwarf_die_from_hash_signature failed: ") +
                           (error != nullptr ? dwarf_errmsg(error) : "<no-error>"));

            // 再退回统一引用解析路径，尽量把可恢复的 type unit 引用吃回来。
            destroy_error(error);
            Dwarf_Off fallback_offset = 0;
            Dwarf_Bool fallback_is_info = true;
            error = nullptr;
            const bool use_manual_ref_sup = form == DW_FORM_ref_sup4 || form == DW_FORM_ref_sup8;
            const int fallback_result = use_manual_ref_sup
                                            ? DW_DLV_NO_ENTRY
                                            : dwarf_global_formref_b(attr, &fallback_offset, &fallback_is_info, &error);
            if (use_manual_ref_sup) {
                const auto manual_offset = read_manual_ref_sup_offset(attr, form);
                if (manual_offset.has_value()) {
                    trace_sig8(signature,
                               "manual ref_sup fallback ok: offset=" +
                                   std::to_string(static_cast<Dwarf_Off>(manual_offset.value())));
                    return ReferenceTarget{.offset = static_cast<Dwarf_Off>(manual_offset.value()), .is_info = true};
                }
            } else if (fallback_result == DW_DLV_OK) {
                trace_sig8(signature,
                           "dwarf_global_formref_b ok: offset=" + std::to_string(fallback_offset) +
                               " is_info=" + std::to_string(fallback_is_info != 0));
                return ReferenceTarget{.offset = fallback_offset, .is_info = fallback_is_info != 0};
            }
            trace_sig8(signature,
                       std::string("dwarf_global_formref_b failed: ") +
                           (error != nullptr ? dwarf_errmsg(error) : "<no-error>"));
            if (debug_file_path != nullptr) {
                const auto& map = debug_types_sig8_map(*debug_file_path);
                const auto iter = map.find(sig8_to_hex(signature));
                if (iter != map.end()) {
                    trace_sig8(signature, "manual .debug_types index ok: offset=" + std::to_string(iter->second));
                }
            }
            destroy_error(error);
            return std::nullopt;
        }

        DieHandle die_handle(debug, referenced_die);
        const auto offset = die_offset(die_handle.get());
        if (!offset.has_value()) {
            trace_sig8(signature, "find_die_given_sig8 returned die without offset");
            return std::nullopt;
        }
        trace_sig8(signature,
                   "dwarf_find_die_given_sig8 ok: offset=" + std::to_string(offset.value()) +
                       " is_info=" + std::to_string(is_info != 0));
        return ReferenceTarget{.offset = offset.value(), .is_info = is_info != 0};
    }

    Dwarf_Off offset = 0;
    Dwarf_Bool is_info = true;
    error = nullptr;
    const int result = dwarf_global_formref_b(attr, &offset, &is_info, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        const auto message = build_error_message("dwarf_global_formref_b failed", error);
        destroy_error(error);
        throw DwarfError(message);
    }
    return ReferenceTarget{.offset = offset, .is_info = is_info != 0};
}

std::optional<DieHandle> die_from_offset(Dwarf_Debug debug, Dwarf_Off offset, bool is_info)
{
    Dwarf_Die die = nullptr;
    Dwarf_Error error = nullptr;
    const int result = dwarf_offdie_b(debug, offset, is_info, &die, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        const auto message = build_error_message("dwarf_offdie_b failed", error);
        destroy_error(error);
        throw DwarfError(message);
    }
    return DieHandle(debug, die);
}

std::optional<Dwarf_Unsigned> unsigned_attr(Dwarf_Attribute attr)
{
    Dwarf_Half form = 0;
    Dwarf_Error form_error = nullptr;
    const int form_result = dwarf_whatform(attr, &form, &form_error);
    if (form_result == DW_DLV_OK && (form == DW_FORM_ref_sup4 || form == DW_FORM_ref_sup8)) {
        const auto offset = read_manual_ref_sup_offset(attr, form);
        if (offset.has_value()) {
            return offset;
        }
    } else if (form_result != DW_DLV_OK && form_result != DW_DLV_NO_ENTRY) {
        destroy_error(form_error);
    }

    Dwarf_Unsigned value = 0;
    Dwarf_Error error = nullptr;
    const int result = dwarf_formudata(attr, &value, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        return std::nullopt;
    }
    return value;
}

std::optional<Dwarf_Signed> signed_attr(Dwarf_Attribute attr)
{
    Dwarf_Signed value = 0;
    Dwarf_Error error = nullptr;
    const int result = dwarf_formsdata(attr, &value, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> signed_const_attr(Dwarf_Attribute attr)
{
    if (const auto signed_value = signed_attr(attr); signed_value.has_value()) {
        return static_cast<std::int64_t>(signed_value.value());
    }
    if (const auto unsigned_value = unsigned_attr(attr); unsigned_value.has_value()) {
        return static_cast<std::int64_t>(unsigned_value.value());
    }
    return std::nullopt;
}

std::optional<std::string> const_value_text_attr(Dwarf_Attribute attr)
{
    Dwarf_Half form = 0;
    Dwarf_Error error = nullptr;
    const int form_result = dwarf_whatform(attr, &form, &error);
    if (form_result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (form_result != DW_DLV_OK) {
        return std::nullopt;
    }

    if (form == DW_FORM_string || form == DW_FORM_strp || form == DW_FORM_line_strp || form == DW_FORM_strp_sup ||
        form == DW_FORM_GNU_strp_alt || form == DW_FORM_strx || form == DW_FORM_strx1 || form == DW_FORM_strx2 ||
        form == DW_FORM_strx3 || form == DW_FORM_strx4) {
        return string_attr(attr);
    }

    if (form == DW_FORM_block || form == DW_FORM_block1 || form == DW_FORM_block2 || form == DW_FORM_block4 ||
        form == DW_FORM_exprloc) {
        Dwarf_Block* block = nullptr;
        const int block_result = dwarf_formblock(attr, &block, &error);
        if (block_result != DW_DLV_OK || block == nullptr) {
            return std::nullopt;
        }
        std::ostringstream stream;
        stream << "0x";
        const auto* bytes = static_cast<const unsigned char*>(block->bl_data);
        for (Dwarf_Unsigned index = 0; index < block->bl_len; ++index) {
            stream << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(bytes[index]);
        }
        dwarf_dealloc(nullptr, block, DW_DLA_BLOCK);
        return stream.str();
    }

    return std::nullopt;
}

std::optional<bool> flag_attr(Dwarf_Attribute attr)
{
    Dwarf_Bool value = false;
    Dwarf_Error error = nullptr;
    const int result = dwarf_formflag(attr, &value, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        return std::nullopt;
    }
    return value != 0;
}

std::optional<std::string> string_attr(Dwarf_Attribute attr)
{
    // 注意：这里虽然会经由 libdwarf 吃到 DW_FORM_strp_sup / DW_FORM_GNU_strp_alt，
    // 但仓内当前只有 .debug_sup section 与 sidecar 自动绑定的正式样本，
    // 还没有覆盖到真正使用 supplementary/alternate string form 的稳定 fixture。
    // 所以这里不能等同于“sup/alt string 已完成回归覆盖”。
    char* value = nullptr;
    Dwarf_Error error = nullptr;
    const int result = dwarf_formstring(attr, &value, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        return std::nullopt;
    }
    return std::string(value);
}

std::optional<Dwarf_Addr> address_attr(Dwarf_Attribute attr)
{
    Dwarf_Addr value = 0;
    Dwarf_Error error = nullptr;
    const int result = dwarf_formaddr(attr, &value, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK) {
        return std::nullopt;
    }
    return value;
}

std::optional<Dwarf_Addr> indexed_address_from_die_location(Dwarf_Die die, const LocationDescription& location)
{
    if (location.kind != DW_LKIND_expression || location.operations.size() != 1) {
        return std::nullopt;
    }

    const auto& op = location.operations.front();
    if (op.atom != DW_OP_addrx && op.atom != DW_OP_GNU_addr_index) {
        return std::nullopt;
    }

    Dwarf_Addr value = 0;
    Dwarf_Error error = nullptr;
    const int result = dwarf_debug_addr_index_to_addr(die, op.operand1, &value, &error);
    if (result != DW_DLV_OK) {
        return std::nullopt;
    }
    return value;
}

namespace {
    // 手工解析 DWARF 位置表达式字节序列，替代 libdwarf 中仅有声明未实现的
    // dwarf_loclist_from_expr_c。按 DWARF 规范逐字节解码操作码与操作数。
    [[nodiscard]] std::vector<LocationOp> decode_expression_ops(const std::uint8_t* data,
                                                                std::size_t length,
                                                                Dwarf_Half address_size)
    {
        std::vector<LocationOp> ops;
        std::size_t offset = 0;

        while (offset < length) {
            const std::uint8_t opcode = data[offset++];
            LocationOp op;
            op.atom = opcode;

            switch (opcode) {
                // ── 零操作数指令 ──
                case DW_OP_deref:       // 0x06
                case DW_OP_dup:         // 0x12
                case DW_OP_drop:        // 0x13
                case DW_OP_over:        // 0x14
                case DW_OP_swap:        // 0x16
                case DW_OP_rot:         // 0x17
                case DW_OP_xderef:      // 0x18
                case DW_OP_abs:         // 0x19
                case DW_OP_and:         // 0x1a
                case DW_OP_div:         // 0x1b
                case DW_OP_minus:       // 0x1c
                case DW_OP_mod:         // 0x1d
                case DW_OP_mul:         // 0x1e
                case DW_OP_neg:         // 0x1f
                case DW_OP_not:         // 0x20
                case DW_OP_or:          // 0x21
                case DW_OP_plus:        // 0x22
                case DW_OP_shl:         // 0x24
                case DW_OP_shr:         // 0x25
                case DW_OP_shra:        // 0x26
                case DW_OP_xor:         // 0x27
                case DW_OP_eq:          // 0x29
                case DW_OP_ge:          // 0x2a
                case DW_OP_gt:          // 0x2b
                case DW_OP_le:          // 0x2c
                case DW_OP_lt:          // 0x2d
                case DW_OP_ne:          // 0x2e
                case DW_OP_nop:         // 0x96
                case DW_OP_stack_value: // 0x9f
                    break;

                // ── DW_OP_constu (0x10): ULEB128 ──
                case DW_OP_constu: {
                    Dwarf_Unsigned uleb_len = 0;
                    Dwarf_Unsigned val = 0;
                    const char* cursor = reinterpret_cast<const char*>(data + offset);
                    const int r =
                        dwarf_decode_leb128(const_cast<char*>(cursor),
                                            &uleb_len,
                                            &val,
                                            reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                    if (r != DW_DLV_OK)
                        return ops;
                    op.operand1 = val;
                    offset += uleb_len;
                    break;
                }

                // ── DW_OP_consts (0x11): SLEB128 ──
                case DW_OP_consts: {
                    Dwarf_Unsigned uleb_result = 0;
                    Dwarf_Signed sleb_result = 0;
                    const char* cursor = reinterpret_cast<const char*>(data + offset);
                    const int dec_result =
                        dwarf_decode_signed_leb128(const_cast<char*>(cursor),
                                                   &uleb_result,
                                                   &sleb_result,
                                                   reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                    if (dec_result != DW_DLV_OK)
                        return ops;
                    op.operand1 = static_cast<Dwarf_Unsigned>(sleb_result);
                    offset += uleb_result;
                    break;
                }

                // ── DW_OP_addr (0x03): address_size 字节地址 ──
                case DW_OP_addr: {
                    if (offset + address_size > length)
                        return ops;
                    Dwarf_Unsigned addr = 0;
                    for (Dwarf_Half i = 0; i < address_size; ++i) {
                        addr |= static_cast<Dwarf_Unsigned>(data[offset + i]) << (i * 8);
                    }
                    op.operand1 = addr;
                    offset += address_size;
                    break;
                }

                // ── DW_OP_const1u (0x08): 1 字节 ──
                case DW_OP_const1u:
                    if (offset >= length)
                        return ops;
                    op.operand1 = data[offset++];
                    break;

                // ── DW_OP_const1s (0x09): 1 字节有符号 ──
                case DW_OP_const1s:
                    if (offset >= length)
                        return ops;
                    op.operand1 = static_cast<Dwarf_Unsigned>(static_cast<std::int8_t>(data[offset++]));
                    break;

                // ── DW_OP_const2u (0x0a): 2 字节 ──
                case DW_OP_const2u:
                    if (offset + 2 > length)
                        return ops;
                    op.operand1 = static_cast<Dwarf_Unsigned>(data[offset]) |
                                  (static_cast<Dwarf_Unsigned>(data[offset + 1]) << 8);
                    offset += 2;
                    break;

                // ── DW_OP_const4u (0x0c): 4 字节 ──
                case DW_OP_const4u:
                    if (offset + 4 > length)
                        return ops;
                    op.operand1 = static_cast<Dwarf_Unsigned>(data[offset]) |
                                  (static_cast<Dwarf_Unsigned>(data[offset + 1]) << 8) |
                                  (static_cast<Dwarf_Unsigned>(data[offset + 2]) << 16) |
                                  (static_cast<Dwarf_Unsigned>(data[offset + 3]) << 24);
                    offset += 4;
                    break;

                // ── lit0..lit31 (0x30-0x4f): 字面量 ──
                case 0x30:
                case 0x31:
                case 0x32:
                case 0x33:
                case 0x34:
                case 0x35:
                case 0x36:
                case 0x37:
                case 0x38:
                case 0x39:
                case 0x3a:
                case 0x3b:
                case 0x3c:
                case 0x3d:
                case 0x3e:
                case 0x3f:
                case 0x40:
                case 0x41:
                case 0x42:
                case 0x43:
                case 0x44:
                case 0x45:
                case 0x46:
                case 0x47:
                case 0x48:
                case 0x49:
                case 0x4a:
                case 0x4b:
                case 0x4c:
                case 0x4d:
                case 0x4e:
                case 0x4f:
                    op.operand1 = opcode - 0x30;
                    break;

                // ── reg0..reg31 (0x50-0x6f): 无操作数，寄存器号 = opcode - 0x50 ──
                case 0x50:
                case 0x51:
                case 0x52:
                case 0x53:
                case 0x54:
                case 0x55:
                case 0x56:
                case 0x57:
                case 0x58:
                case 0x59:
                case 0x5a:
                case 0x5b:
                case 0x5c:
                case 0x5d:
                case 0x5e:
                case 0x5f:
                case 0x60:
                case 0x61:
                case 0x62:
                case 0x63:
                case 0x64:
                case 0x65:
                case 0x66:
                case 0x67:
                case 0x68:
                case 0x69:
                case 0x6a:
                case 0x6b:
                case 0x6c:
                case 0x6d:
                case 0x6e:
                case 0x6f:
                    op.operand1 = opcode - 0x50;
                    break;

                // ── breg0..breg31 (0x70-0x8f): SLEB128 偏移 ──
                case 0x70:
                case 0x71:
                case 0x72:
                case 0x73:
                case 0x74:
                case 0x75:
                case 0x76:
                case 0x77:
                case 0x78:
                case 0x79:
                case 0x7a:
                case 0x7b:
                case 0x7c:
                case 0x7d:
                case 0x7e:
                case 0x7f:
                case 0x80:
                case 0x81:
                case 0x82:
                case 0x83:
                case 0x84:
                case 0x85:
                case 0x86:
                case 0x87:
                case 0x88:
                case 0x89:
                case 0x8a:
                case 0x8b:
                case 0x8c:
                case 0x8d:
                case 0x8e:
                case 0x8f: {
                    op.operand1 = opcode - 0x70;
                    Dwarf_Unsigned uleb_result = 0;
                    Dwarf_Signed sleb_result = 0;
                    const char* cursor = reinterpret_cast<const char*>(data + offset);
                    const int dec_result =
                        dwarf_decode_signed_leb128(const_cast<char*>(cursor),
                                                   &uleb_result,
                                                   &sleb_result,
                                                   reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                    if (dec_result != DW_DLV_OK)
                        return ops;
                    op.operand2 = static_cast<Dwarf_Unsigned>(sleb_result);
                    offset += uleb_result;
                    break;
                }

                // ── DW_OP_fbreg (0x91): SLEB128 偏移 ──
                case DW_OP_fbreg: {
                    Dwarf_Unsigned uleb_result = 0;
                    Dwarf_Signed sleb_result = 0;
                    const char* cursor = reinterpret_cast<const char*>(data + offset);
                    const int dec_result =
                        dwarf_decode_signed_leb128(const_cast<char*>(cursor),
                                                   &uleb_result,
                                                   &sleb_result,
                                                   reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                    if (dec_result != DW_DLV_OK)
                        return ops;
                    op.operand1 = static_cast<Dwarf_Unsigned>(sleb_result);
                    offset += uleb_result;
                    break;
                }

                // ── DW_OP_bregx (0x92): ULEB128 寄存器号 + SLEB128 偏移 ──
                case DW_OP_bregx: {
                    Dwarf_Unsigned len1 = 0;
                    Dwarf_Unsigned val1 = 0;
                    {
                        const char* c1 = reinterpret_cast<const char*>(data + offset);
                        const int r1 =
                            dwarf_decode_leb128(const_cast<char*>(c1),
                                                &len1,
                                                &val1,
                                                reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                        if (r1 != DW_DLV_OK)
                            return ops;
                    }
                    op.operand1 = val1;
                    offset += len1;

                    Dwarf_Unsigned len2 = 0;
                    Dwarf_Signed val2 = 0;
                    {
                        const char* c2 = reinterpret_cast<const char*>(data + offset);
                        const int r2 = dwarf_decode_signed_leb128(
                            const_cast<char*>(c2),
                            &len2,
                            &val2,
                            reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                        if (r2 != DW_DLV_OK)
                            return ops;
                    }
                    op.operand2 = static_cast<Dwarf_Unsigned>(val2);
                    offset += len2;
                    break;
                }

                // ── DW_OP_regx (0x90): ULEB128 寄存器号 ──
                case DW_OP_regx: {
                    Dwarf_Unsigned uleb_len = 0;
                    Dwarf_Unsigned val = 0;
                    const char* cursor = reinterpret_cast<const char*>(data + offset);
                    const int r =
                        dwarf_decode_leb128(const_cast<char*>(cursor),
                                            &uleb_len,
                                            &val,
                                            reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                    if (r != DW_DLV_OK)
                        return ops;
                    op.operand1 = val;
                    offset += uleb_len;
                    break;
                }

                // ── DW_OP_piece (0x93): ULEB128 ──
                case DW_OP_piece: {
                    Dwarf_Unsigned uleb_len = 0;
                    Dwarf_Unsigned val = 0;
                    const char* cursor = reinterpret_cast<const char*>(data + offset);
                    const int r =
                        dwarf_decode_leb128(const_cast<char*>(cursor),
                                            &uleb_len,
                                            &val,
                                            reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                    if (r != DW_DLV_OK)
                        return ops;
                    op.operand1 = val;
                    offset += uleb_len;
                    break;
                }

                // ── DW_OP_bit_piece (0x94): ULEB128 size + ULEB128 offset ──
                case DW_OP_bit_piece: {
                    Dwarf_Unsigned len1 = 0;
                    Dwarf_Unsigned val1 = 0;
                    {
                        const char* c1 = reinterpret_cast<const char*>(data + offset);
                        const int r1 =
                            dwarf_decode_leb128(const_cast<char*>(c1),
                                                &len1,
                                                &val1,
                                                reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                        if (r1 != DW_DLV_OK)
                            return ops;
                    }
                    op.operand1 = val1;
                    offset += len1;

                    Dwarf_Unsigned len2 = 0;
                    Dwarf_Unsigned val2 = 0;
                    {
                        const char* c2 = reinterpret_cast<const char*>(data + offset);
                        const int r2 =
                            dwarf_decode_leb128(const_cast<char*>(c2),
                                                &len2,
                                                &val2,
                                                reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                        if (r2 != DW_DLV_OK)
                            return ops;
                    }
                    op.operand2 = val2;
                    offset += len2;
                    break;
                }

                // ── DW_OP_plus_uconst (0x23): ULEB128 ──
                case DW_OP_plus_uconst: {
                    Dwarf_Unsigned uleb_len = 0;
                    Dwarf_Unsigned val = 0;
                    const char* cursor = reinterpret_cast<const char*>(data + offset);
                    const int r =
                        dwarf_decode_leb128(const_cast<char*>(cursor),
                                            &uleb_len,
                                            &val,
                                            reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                    if (r != DW_DLV_OK)
                        return ops;
                    op.operand1 = val;
                    offset += uleb_len;
                    break;
                }

                // ── DW_OP_addrx (0xa1): ULEB128 索引 ──
                case DW_OP_addrx: {
                    Dwarf_Unsigned uleb_len = 0;
                    Dwarf_Unsigned val = 0;
                    const char* cursor = reinterpret_cast<const char*>(data + offset);
                    const int r =
                        dwarf_decode_leb128(const_cast<char*>(cursor),
                                            &uleb_len,
                                            &val,
                                            reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                    if (r != DW_DLV_OK)
                        return ops;
                    op.operand1 = val;
                    offset += uleb_len;
                    break;
                }

                // ── DW_OP_GNU_addr_index (0xfb): ULEB128 索引 ──
                case 0xfb: {
                    Dwarf_Unsigned uleb_len = 0;
                    Dwarf_Unsigned val = 0;
                    const char* cursor = reinterpret_cast<const char*>(data + offset);
                    const int r =
                        dwarf_decode_leb128(const_cast<char*>(cursor),
                                            &uleb_len,
                                            &val,
                                            reinterpret_cast<char*>(const_cast<std::uint8_t*>(data + length)));
                    if (r != DW_DLV_OK)
                        return ops;
                    op.operand1 = val;
                    offset += uleb_len;
                    break;
                }

                default:
                    // 无法识别的操作码，跳过此字节继续解析后续操作
                    break;
            }

            assign_location_op_name(op);
            ops.push_back(std::move(op));
        }

        return ops;
    }

    [[nodiscard]] std::optional<LocationDescription> read_location_list_head(Dwarf_Loc_Head_c head,
                                                                             Dwarf_Unsigned entry_count)
    {
        if (head == nullptr) {
            return std::nullopt;
        }

        Dwarf_Error error = nullptr;
        LocationDescription description;
        description.entry_count = static_cast<std::uint64_t>(entry_count);
        if (dwarf_get_loclist_head_kind(head, &description.kind, &error) != DW_DLV_OK) {
            return std::nullopt;
        }
        for (Dwarf_Unsigned entry_index = 0; entry_index < entry_count; ++entry_index) {
            LocationDescription::Entry entry;
            Dwarf_Small lle_value = 0;
            Dwarf_Addr raw_lowpc = 0;
            Dwarf_Addr raw_hipc = 0;
            Dwarf_Bool debug_addr_unavailable = false;
            Dwarf_Addr lowpc_cooked = 0;
            Dwarf_Addr hipc_cooked = 0;
            Dwarf_Unsigned loclist_expr_op_count = 0;
            Dwarf_Locdesc_c locdesc = nullptr;
            Dwarf_Small loclist_source = 0;
            Dwarf_Unsigned expression_offset = 0;
            Dwarf_Unsigned locdesc_offset = 0;
            const int entry_result = dwarf_get_locdesc_entry_d(head,
                                                               entry_index,
                                                               &lle_value,
                                                               &raw_lowpc,
                                                               &raw_hipc,
                                                               &debug_addr_unavailable,
                                                               &lowpc_cooked,
                                                               &hipc_cooked,
                                                               &loclist_expr_op_count,
                                                               &locdesc,
                                                               &loclist_source,
                                                               &expression_offset,
                                                               &locdesc_offset,
                                                               &error);
            if (entry_result == DW_DLV_OK) {
                entry.debug_addr_unavailable = debug_addr_unavailable != 0;
                const bool has_pc_range = lle_value == DW_LLE_offset_pair || lle_value == DW_LLE_start_end ||
                                          lle_value == DW_LLE_start_length || lle_value == DW_LLE_startx_endx ||
                                          lle_value == DW_LLE_startx_length;
                if (lle_value == DW_LLE_offset_pair || lle_value == DW_LLE_start_end ||
                    lle_value == DW_LLE_start_length || lle_value == DW_LLE_startx_endx ||
                    lle_value == DW_LLE_startx_length) {
                    entry.raw_low_pc = raw_lowpc;
                }
                if (lle_value == DW_LLE_offset_pair || lle_value == DW_LLE_start_end ||
                    lle_value == DW_LLE_startx_endx) {
                    entry.raw_high_pc = raw_hipc;
                }
                if (lle_value == DW_LLE_start_length || lle_value == DW_LLE_startx_length) {
                    entry.raw_high_pc = raw_lowpc + raw_hipc;
                }
                if (!entry.debug_addr_unavailable && has_pc_range) {
                    entry.cooked_low_pc = lowpc_cooked;
                    entry.cooked_high_pc = hipc_cooked;
                }
                for (Dwarf_Unsigned index = 0; index < loclist_expr_op_count; ++index) {
                    LocationOp op;
                    Dwarf_Unsigned offset_for_branch = 0;
                    const int op_result = dwarf_get_location_op_value_c(
                        locdesc, index, &op.atom, &op.operand1, &op.operand2, &op.operand3, &offset_for_branch, &error);
                    if (op_result == DW_DLV_OK) {
                        assign_location_op_name(op);
                        entry.operations.push_back(op);
                        description.operations.push_back(op);
                    }
                }
                if (has_pc_range || !entry.operations.empty()) {
                    description.entries.push_back(std::move(entry));
                }
            }
        }
        return description;
    }

    [[nodiscard]] bool is_location_expression_form(const Dwarf_Half form)
    {
        return form == DW_FORM_block || form == DW_FORM_block1 || form == DW_FORM_block2 || form == DW_FORM_block4 ||
               form == DW_FORM_exprloc;
    }

} // namespace

std::optional<LocationDescription> read_location_expression(Dwarf_Debug debug,
                                                            Dwarf_Ptr expression_data,
                                                            Dwarf_Unsigned expression_length,
                                                            Dwarf_Half address_size,
                                                            Dwarf_Half offset_size,
                                                            Dwarf_Half dwarf_version)
{
    (void) offset_size;
    (void) dwarf_version;
    if (debug == nullptr || expression_data == nullptr) {
        return std::nullopt;
    }

    // libdwarf 当前版本 (2.3.1-33) 的 dwarf_loclist_from_expr_c 仅有声明而无实现，
    // 因此改用手工解析 DWARF 表达式字节序列。
    const auto* bytes = static_cast<const std::uint8_t*>(expression_data);
    auto ops = decode_expression_ops(bytes, static_cast<std::size_t>(expression_length), address_size);
    LocationDescription description;
    description.kind = DW_LKIND_expression;
    description.entry_count = 1;

    LocationDescription::Entry entry;
    entry.operations = ops;
    entry.raw_low_pc = 0;
    entry.raw_high_pc = 0;
    description.entries.push_back(std::move(entry));
    description.operations = std::move(ops);

    return description;
}

std::optional<LocationDescription> read_location_description(
    Dwarf_Debug debug, Dwarf_Attribute attr, Dwarf_Half address_size, Dwarf_Half offset_size, Dwarf_Half dwarf_version)
{
    Dwarf_Half form = 0;
    Dwarf_Error form_error = nullptr;
    const int form_result = dwarf_whatform(attr, &form, &form_error);
    if (form_result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (form_result != DW_DLV_OK) {
        return std::nullopt;
    }

    if (is_location_expression_form(form)) {
        // DW_FORM_block* / exprloc 保存的是位置表达式字节，不是 .debug_loc/.debug_loclists 引用。
        // TI-COFF DWARF2 会用 DW_FORM_block1 + DW_OP_addr 表达静态变量地址，不能送入 dwarf_get_loclist_c。
        if (form == DW_FORM_exprloc) {
            Dwarf_Unsigned expression_length = 0;
            Dwarf_Ptr expression_data = nullptr;
            Dwarf_Error error = nullptr;
            const int expr_result = dwarf_formexprloc(attr, &expression_length, &expression_data, &error);
            if (expr_result != DW_DLV_OK) {
                return std::nullopt;
            }
            return read_location_expression(
                debug, expression_data, expression_length, address_size, offset_size, dwarf_version);
        }

        Dwarf_Block* block = nullptr;
        Dwarf_Error error = nullptr;
        const int block_result = dwarf_formblock(attr, &block, &error);
        if (block_result != DW_DLV_OK || block == nullptr) {
            return std::nullopt;
        }
        return read_location_expression(debug, block->bl_data, block->bl_len, address_size, offset_size, dwarf_version);
    }

    Dwarf_Loc_Head_c head = nullptr;
    Dwarf_Unsigned entry_count = 0;
    Dwarf_Error error = nullptr;
    const int result = dwarf_get_loclist_c(attr, &head, &entry_count, &error);
    if (result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (result != DW_DLV_OK || head == nullptr) {
        return std::nullopt;
    }
    auto description = read_location_list_head(head, entry_count);
    dwarf_dealloc_loc_head_c(head);
    return description;
}

std::optional<LocationDescription> read_range_description(Dwarf_Attribute attr, Dwarf_Die owner_die)
{
    Dwarf_Half form = 0;
    Dwarf_Error error = nullptr;
    const int form_result = dwarf_whatform(attr, &form, &error);
    if (form_result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (form_result != DW_DLV_OK) {
        return std::nullopt;
    }

    Dwarf_Unsigned index_or_offset = 0;
    if (form == DW_FORM_rnglistx) {
        const int value_result = dwarf_formudata(attr, &index_or_offset, &error);
        if (value_result != DW_DLV_OK) {
            return std::nullopt;
        }
    } else if (form == DW_FORM_sec_offset) {
        const int value_result = dwarf_formudata(attr, &index_or_offset, &error);
        if (value_result != DW_DLV_OK) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }

    Dwarf_Rnglists_Head head = nullptr;
    Dwarf_Unsigned entry_count = 0;
    Dwarf_Unsigned global_offset = 0;
    error = nullptr;
    const int head_result =
        dwarf_rnglists_get_rle_head(attr, form, index_or_offset, &head, &entry_count, &global_offset, &error);
    if (head_result == DW_DLV_NO_ENTRY) {
        return std::nullopt;
    }
    if (head_result != DW_DLV_OK || head == nullptr) {
        return std::nullopt;
    }

    LocationDescription description;
    description.kind = static_cast<unsigned int>(form);
    description.entry_count = static_cast<std::uint64_t>(entry_count);

    for (Dwarf_Unsigned entry_index = 0; entry_index < entry_count; ++entry_index) {
        unsigned int entry_length = 0;
        unsigned int rle_value = 0;
        Dwarf_Unsigned raw1 = 0;
        Dwarf_Unsigned raw2 = 0;
        Dwarf_Bool debug_addr_unavailable = false;
        Dwarf_Unsigned cooked1 = 0;
        Dwarf_Unsigned cooked2 = 0;
        error = nullptr;
        const int entry_result = dwarf_get_rnglists_entry_fields_a(head,
                                                                   entry_index,
                                                                   &entry_length,
                                                                   &rle_value,
                                                                   &raw1,
                                                                   &raw2,
                                                                   &debug_addr_unavailable,
                                                                   &cooked1,
                                                                   &cooked2,
                                                                   &error);
        if (entry_result != DW_DLV_OK) {
            continue;
        }

        LocationDescription::Entry entry;
        entry.debug_addr_unavailable = debug_addr_unavailable != 0;

        switch (rle_value) {
            case DW_RLE_base_address:
            case DW_RLE_base_addressx:
                if (!entry.debug_addr_unavailable) {
                    entry.cooked_low_pc = cooked1;
                    entry.cooked_high_pc = cooked1;
                } else if (rle_value == DW_RLE_base_addressx && owner_die != nullptr) {
                    Dwarf_Addr resolved = 0;
                    Dwarf_Error addr_error = nullptr;
                    const int addr_result = dwarf_debug_addr_index_to_addr(owner_die, raw1, &resolved, &addr_error);
                    if (addr_result == DW_DLV_OK) {
                        entry.debug_addr_unavailable = false;
                        entry.cooked_low_pc = resolved;
                        entry.cooked_high_pc = resolved;
                    } else if (addr_error != nullptr) {
                        destroy_error(addr_error);
                    }
                } else {
                    entry.raw_low_pc = raw1;
                    entry.raw_high_pc = raw1;
                }
                break;
            case DW_RLE_offset_pair:
            case DW_RLE_start_end:
            case DW_RLE_startx_endx:
                entry.raw_low_pc = raw1;
                entry.raw_high_pc = raw2;
                if (!entry.debug_addr_unavailable) {
                    entry.cooked_low_pc = cooked1;
                    entry.cooked_high_pc = cooked2;
                } else if (rle_value == DW_RLE_startx_endx && owner_die != nullptr) {
                    Dwarf_Addr resolved_low = 0;
                    Dwarf_Addr resolved_high = 0;
                    Dwarf_Error low_error = nullptr;
                    const int low_result = dwarf_debug_addr_index_to_addr(owner_die, raw1, &resolved_low, &low_error);
                    if (low_result != DW_DLV_OK && low_error != nullptr) {
                        destroy_error(low_error);
                    }
                    Dwarf_Error high_error = nullptr;
                    const int high_result =
                        dwarf_debug_addr_index_to_addr(owner_die, raw2, &resolved_high, &high_error);
                    if (high_result != DW_DLV_OK && high_error != nullptr) {
                        destroy_error(high_error);
                    }
                    if (low_result == DW_DLV_OK && high_result == DW_DLV_OK) {
                        entry.debug_addr_unavailable = false;
                        entry.cooked_low_pc = resolved_low;
                        entry.cooked_high_pc = resolved_high;
                    }
                }
                break;
            case DW_RLE_start_length:
            case DW_RLE_startx_length:
                entry.raw_low_pc = raw1;
                entry.raw_high_pc = raw1 + raw2;
                if (!entry.debug_addr_unavailable) {
                    entry.cooked_low_pc = cooked1;
                    entry.cooked_high_pc = cooked1 + cooked2;
                } else if (rle_value == DW_RLE_startx_length && owner_die != nullptr) {
                    Dwarf_Addr resolved = 0;
                    Dwarf_Error addr_error = nullptr;
                    const int addr_result = dwarf_debug_addr_index_to_addr(owner_die, raw1, &resolved, &addr_error);
                    if (addr_result == DW_DLV_OK) {
                        entry.debug_addr_unavailable = false;
                        entry.cooked_low_pc = resolved;
                        entry.cooked_high_pc = resolved + raw2;
                    } else if (addr_error != nullptr) {
                        destroy_error(addr_error);
                    }
                }
                break;
            default:
                break;
        }

        description.entries.push_back(std::move(entry));
    }

    dwarf_dealloc_rnglists_head(head);
    return description;
}

} // namespace elf_static_view::elf
