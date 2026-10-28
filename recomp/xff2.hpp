#pragma once

#include "snr2.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace recomp {

constexpr std::uint32_t kXff2Magic = 0x32666678u;  // "xff2"
// The value of an external jump link that is no symbol but an absolute address
constexpr std::uint32_t kAbsolute = 1;

// The "xff2" format is worked out from the files of one game and from the code
// of its loader, both in the owner's disc image; no other source was read. It
// is an ELF REL object whose tables the loader keeps in memory next to the
// code (docs/sotc.md). The relocation rules (R_MIPS_32, _26, _HI16, _LO16, a
// HI16 paired with the next LO16) are the MIPS ELF ABI's, which is public.
// This file is a header only so that no build file has to list it.

namespace xff2_detail {

// The file's pointers are file offsets plus this.
constexpr std::uint32_t kLinkBase = 0x40010000u;
constexpr std::uint32_t kProgbits = 1u;
constexpr std::uint32_t kOverlayTable = 0x7FFFF420u;
constexpr std::uint32_t kRelSection = 9u;

inline std::uint32_t u32(const std::vector<std::uint8_t> &b, std::uint32_t o) {
    std::uint32_t v = 0;
    if (o <= b.size() && b.size() - o >= 4) std::memcpy(&v, b.data() + o, 4);
    return v;
}

inline void put32(std::vector<std::uint8_t> &b, std::uint32_t o, std::uint32_t v) {
    if (o <= b.size() && b.size() - o >= 4) std::memcpy(b.data() + o, &v, 4);
}

struct Section {
    std::uint32_t addr{}, size{}, type{}, offset{};
    std::string name;
    bool placed{};  // part of the image at its own file offset
    std::uint32_t rel() const { return addr - kLinkBase; }
};

inline std::string cstring(const std::vector<std::uint8_t> &b, std::uint32_t at) {
    std::string s;
    for (std::uint32_t o = at; o < b.size() && b[o] && s.size() < 64; ++o) s += static_cast<char>(b[o]);
    return s;
}

inline std::string clean_name(const std::string &name) {
    std::string stem = name.substr(0, name.find('.'));
    for (char &ch : stem)
        if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
    return stem.empty() ? "xff" : stem;
}

}

// Reads one .XFF file, with the name the module is known by. The module comes
// back the way a relocatable module of the SNR2 kind does: its image is the
// file as it lies in memory when the loader reads it to some base, relocated to
// base 0. Only references the file resolves itself are rebased; the rest
// (symbols the loader links to other modules, and its .bss) stay links that
// read the word the loader left in memory.
inline std::optional<Snr2Module> read_xff2_module(std::vector<std::uint8_t> bytes, const std::string &name) {
    using namespace xff2_detail;
    if (bytes.size() < 0x100 || u32(bytes, 0) != kXff2Magic) return std::nullopt;
    const std::uint32_t file_size = u32(bytes, 0x14);
    if (file_size > bytes.size()) return std::nullopt;
    const auto offset_of = [&](std::uint32_t header_field) -> std::optional<std::uint32_t> {
        const std::uint32_t v = u32(bytes, header_field);
        if (v < kLinkBase || v - kLinkBase >= file_size) return std::nullopt;
        return v - kLinkBase;
    };
    const auto symtab = offset_of(0x28), section_table = offset_of(0x30), rel_table = offset_of(0x3C), name_offsets = offset_of(0x44),
               names = offset_of(0x48);
    const std::uint32_t nsym = u32(bytes, 0x24), nrel = u32(bytes, 0x38), nsec = u32(bytes, 0x40);
    if (!symtab || !section_table || !rel_table || !name_offsets || !names || nsec == 0 || nsec > 0x400 || nrel > 0x400) return std::nullopt;
    if (*section_table + nsec * 32ull > file_size || *rel_table + nrel * 28ull > file_size || *symtab + nsym * 16ull > file_size ||
        *name_offsets + nsec * 4ull > file_size)
        return std::nullopt;

    std::vector<Section> sections(nsec);
    std::uint32_t text = 0;
    for (std::uint32_t i = 0; i < nsec; ++i) {
        const std::uint32_t o = *section_table + i * 32u;
        Section &s = sections[i];
        s.addr = u32(bytes, o + 4);
        s.size = u32(bytes, o + 8);
        s.type = u32(bytes, o + 16);
        s.offset = u32(bytes, o + 28);
        s.name = cstring(bytes, *names + u32(bytes, *name_offsets + i * 4u));
        s.placed = (s.type == kProgbits || s.type == kOverlayTable) && s.addr >= kLinkBase && s.rel() == s.offset && s.offset + s.size <= file_size;
        if (!text && s.placed && s.name == ".text" && s.size) text = i;
    }
    if (!text) return std::nullopt;

    Snr2Module m;
    m.name = clean_name(name);
    m.magic = kXff2Magic;
    m.size = file_size;
    m.code_begin = sections[text].offset;
    m.code_end = sections[text].offset + sections[text].size;
    const auto in_text = [&](std::uint32_t a) { return a >= m.code_begin && a < m.code_end && !(a & 3u); };

    // A symbol is defined in a section of the file (an address relative to the file's base), is absolute (the
    // boot program's own symbols, which the file's linker knew the addresses of), or is left to the loader.
    struct Target {
        bool internal{};
        bool absolute{};
        std::uint32_t address{};
    };
    const auto target_of = [&](std::uint32_t sym) {
        Target t;
        if (sym >= nsym) return t;
        const std::uint32_t o = *symtab + sym * 16u;
        const std::uint32_t shndx = static_cast<std::uint16_t>(bytes[o + 14] | bytes[o + 15] << 8);
        if (shndx == 0xFFF1u) {
            t.absolute = true;
            t.address = u32(bytes, o + 4);
            return t;
        }
        if (shndx == 0 || shndx >= nsec || !sections[shndx].placed) return t;
        t.internal = true;
        t.address = sections[shndx].offset + u32(bytes, o + 4);
        return t;
    };

    for (std::uint32_t sym = 0; sym < nsym; ++sym) {
        const std::uint32_t o = *symtab + sym * 16u;
        const Target t = target_of(sym);
        if (t.internal && (bytes[o + 12] & 15u) == 2u && in_text(t.address)) m.seeds.push_back(t.address);
        if (t.absolute && (bytes[o + 12] & 15u) == 2u && !(t.address & 3u)) m.program_entries.push_back(t.address);
    }
    if (const std::uint32_t entry = m.code_begin + u32(bytes, 0x4C); in_text(entry)) m.seeds.push_back(entry);

    for (std::uint32_t r = 0; r < nrel; ++r) {
        const std::uint32_t e = *rel_table + r * 28u;
        const std::uint32_t kind = u32(bytes, e), count = u32(bytes, e + 4), target_section = u32(bytes, e + 8);
        const std::uint32_t rels = u32(bytes, e + 20), originals = u32(bytes, e + 24);
        if (kind != kRelSection || !count || target_section >= nsec || !sections[target_section].placed) continue;
        if (rels + count * 8ull > file_size || originals + count * 8ull > file_size) continue;
        const bool in_code = target_section == text;
        const std::uint32_t section_at = sections[target_section].offset;
        const auto type_of = [&](std::uint32_t k) { return u32(bytes, rels + k * 8u + 4u) & 0xFFu; };
        for (std::uint32_t k = 0; k < count; ++k) {
            const std::uint32_t info = u32(bytes, rels + k * 8u + 4u);
            const std::uint32_t site = section_at + u32(bytes, rels + k * 8u);
            const std::uint32_t original = u32(bytes, originals + k * 8u);
            const Target t = target_of(info >> 8);
            if (site + 4 > file_size || (site & 3u)) continue;
            switch (info & 0xFFu) {
            case 2:
                if (t.absolute) put32(bytes, site, original + t.address);
                if (t.internal) {
                    put32(bytes, site, original + t.address);
                    if (!in_code && in_text(original + t.address)) m.seeds.push_back(original + t.address);
                }
                if (in_code) m.links[site] = {Fix::Word, !t.internal, 0};
                break;
            case 4: {
                if (!in_code) break;
                if (t.internal || t.absolute) {
                    const std::uint32_t word = original + ((t.address >> 2) & 0x03FFFFFFu);
                    put32(bytes, site, word);
                    if (t.internal) m.seeds.push_back((word & 0x03FFFFFFu) << 2);
                }
                m.links[site] = {Fix::Jump, !t.internal, t.absolute ? kAbsolute : 0u};
                break;
            }
            case 5: {
                if (!in_code) break;
                std::uint32_t partner = k + 1;
                while (partner < count && type_of(partner) == 5) ++partner;
                if (!(t.internal || t.absolute) || partner >= count || type_of(partner) != 6) {
                    m.links[site] = {Fix::High, true, 0};
                    break;
                }
                const std::uint32_t low = u32(bytes, originals + partner * 8u) & 0xFFFFu;
                const std::uint32_t value = ((original & 0xFFFFu) << 16) + static_cast<std::uint32_t>(static_cast<std::int16_t>(low)) + t.address;
                put32(bytes, site, (original & 0xFFFF0000u) | (((value + 0x8000u) >> 16) & 0xFFFFu));
                if (t.absolute) break;
                m.links[site] = {Fix::High, false, value};
                if (in_text(value)) m.seeds.push_back(value);
                break;
            }
            case 6:
                if (!in_code) break;
                if (t.internal || t.absolute) put32(bytes, site, (original & 0xFFFF0000u) | ((original + t.address) & 0xFFFFu));
                if (!t.absolute) m.links[site] = {Fix::Low, !t.internal, 0};
                break;
            default:
                break;
            }
        }
    }

    // The linker of these files resolved calls into the game's boot program
    // itself: a j or jal that carries no relocation holds an absolute address
    // that is the same wherever the file is loaded, so it is a call out of the
    // module like one to a symbol the loader links, and the word in memory is
    // the one to follow.
    for (std::uint32_t site = m.code_begin; site + 4 <= m.code_end; site += 4)
        if (const std::uint32_t op = u32(bytes, site) >> 26; (op == 2u || op == 3u) && !m.links.count(site)) m.links[site] = {Fix::Jump, true, kAbsolute};

    std::sort(m.seeds.begin(), m.seeds.end());
    m.seeds.erase(std::unique(m.seeds.begin(), m.seeds.end()), m.seeds.end());
    m.seeds.erase(std::remove_if(m.seeds.begin(), m.seeds.end(), [&](std::uint32_t a) { return !in_text(a); }), m.seeds.end());
    m.bytes = std::move(bytes);
    m.bytes.resize(file_size);
    return m;
}

}
