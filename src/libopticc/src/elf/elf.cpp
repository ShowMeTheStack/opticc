// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/elf/elf.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace opticc::elf {
namespace {

struct Reader {
  const std::vector<uint8_t>& b;
  const std::string& path;
  [[noreturn]] void fail(const std::string& m) const { throw std::runtime_error(path + ": " + m); }
  template <class T> T at(size_t off) const {
    if (off + sizeof(T) > b.size()) fail("truncated at offset " + std::to_string(off));
    T v; std::memcpy(&v, b.data() + off, sizeof(T)); return v;   // host is little-endian on every target we support
  }
  std::string str(size_t base, size_t limit, uint32_t off) const {
    if (base + off >= limit) fail("string offset out of range");
    const char* p = reinterpret_cast<const char*>(b.data() + base + off);
    size_t n = strnlen(p, limit - base - off);
    return std::string(p, n);
  }
};

struct RawShdr { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t addralign, entsize; };

template <class T> void put(std::vector<uint8_t>& v, T x) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&x); v.insert(v.end(), p, p + sizeof(T)); }
void put_at(std::vector<uint8_t>& v, size_t off, uint64_t x, size_t n) { std::memcpy(v.data() + off, &x, n); }
void align_to(std::vector<uint8_t>& v, uint64_t a) { if (a < 2) return; while (v.size() % a) v.push_back(0); }

}  // namespace

bool is_pcrel_type(uint32_t t) { return t == R_X86_64_PC32 || t == R_X86_64_PLT32 || t == R_X86_64_PC64 || t == R_X86_64_PC16 || t == R_X86_64_PC8; }
bool is_got_type(uint32_t t) { return t == R_X86_64_GOT32 || t == R_X86_64_GOTPCREL || t == R_X86_64_GOTOFF64 || t == R_X86_64_GOTPC32 || t == R_X86_64_GOTPCRELX || t == R_X86_64_REX_GOTPCRELX || t == R_X86_64_CODE_4_GOTPCRELX || (t >= 16 && t <= 23) /*TLS GOT forms*/; }
const char* reloc_name(uint32_t t) {
  switch (t) {
    case R_X86_64_NONE: return "R_X86_64_NONE"; case R_X86_64_64: return "R_X86_64_64"; case R_X86_64_PC32: return "R_X86_64_PC32";
    case R_X86_64_GOT32: return "R_X86_64_GOT32"; case R_X86_64_PLT32: return "R_X86_64_PLT32"; case R_X86_64_GOTPCREL: return "R_X86_64_GOTPCREL";
    case R_X86_64_32: return "R_X86_64_32"; case R_X86_64_32S: return "R_X86_64_32S"; case R_X86_64_16: return "R_X86_64_16";
    case R_X86_64_PC16: return "R_X86_64_PC16"; case R_X86_64_8: return "R_X86_64_8"; case R_X86_64_PC8: return "R_X86_64_PC8";
    case R_X86_64_PC64: return "R_X86_64_PC64"; case R_X86_64_GOTOFF64: return "R_X86_64_GOTOFF64"; case R_X86_64_GOTPC32: return "R_X86_64_GOTPC32";
    case R_X86_64_GOTPCRELX: return "R_X86_64_GOTPCRELX"; case R_X86_64_REX_GOTPCRELX: return "R_X86_64_REX_GOTPCRELX";
    case R_X86_64_CODE_4_GOTPCRELX: return "R_X86_64_CODE_4_GOTPCRELX"; default: return "R_X86_64_OTHER";
  }
}
std::string section_name_of(const Object& o, const Symbol& s) {
  if (s.shndx >= 0) return o.sections[static_cast<size_t>(s.shndx)].name;
  if (s.special == SHN_ABS) return "*ABS*"; if (s.special == SHN_COMMON) return "*COM*"; return "*UND*";
}

Object read_object(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return parse_object(bytes, path);
}

Object parse_object(const std::vector<uint8_t>& bytes, const std::string& path) {
  Reader r{bytes, path};
  if (bytes.size() < 64 || std::memcmp(bytes.data(), "\x7f" "ELF", 4) != 0) r.fail("not an ELF file");
  if (bytes[4] != 2) r.fail("not ELFCLASS64");
  if (bytes[5] != 1) r.fail("not little-endian");
  uint16_t e_type = r.at<uint16_t>(16), e_machine = r.at<uint16_t>(18);
  if (e_type != 1) r.fail("not a relocatable object (ET_REL)");
  if (e_machine != 62) r.fail("not x86-64 (EM_X86_64)");
  Object o; o.path = path; o.osabi = bytes[7]; o.eflags = r.at<uint32_t>(48);
  uint64_t shoff = r.at<uint64_t>(40); uint16_t shentsize = r.at<uint16_t>(58), shnum = r.at<uint16_t>(60), shstrndx = r.at<uint16_t>(62);
  if (shentsize != 64) r.fail("unexpected e_shentsize");
  if (shnum == 0) r.fail("no section headers");
  std::vector<RawShdr> sh(shnum);
  for (uint16_t i = 0; i < shnum; ++i) {
    size_t off = shoff + 64ull * i;
    sh[i] = {r.at<uint32_t>(off), r.at<uint32_t>(off + 4), r.at<uint64_t>(off + 8), r.at<uint64_t>(off + 16), r.at<uint64_t>(off + 24),
             r.at<uint64_t>(off + 32), r.at<uint32_t>(off + 40), r.at<uint32_t>(off + 44), r.at<uint64_t>(off + 48), r.at<uint64_t>(off + 56)};
  }
  if (shstrndx >= shnum) r.fail("bad e_shstrndx");
  auto secname = [&](uint32_t nameoff) { return r.str(sh[shstrndx].offset, sh[shstrndx].offset + sh[shstrndx].size, nameoff); };
  // locate the symbol table
  int symtab = -1;
  for (uint16_t i = 0; i < shnum; ++i) if (sh[i].type == SHT_SYMTAB) { if (symtab >= 0) r.fail("multiple symbol tables"); symtab = i; }
  if (symtab < 0) r.fail("no symbol table");
  uint32_t strtab = sh[symtab].link;
  // decide which original sections survive into the model
  std::vector<int> map(shnum, -1);
  o.sections.push_back(Section{});  // null
  for (uint16_t i = 1; i < shnum; ++i) {
    const auto& s = sh[i];
    if (s.type == SHT_REL) r.fail("SHT_REL relocations are not supported (x86-64 uses RELA)");
    if (s.type == SHT_SYMTAB_SHNDX) r.fail("SHT_SYMTAB_SHNDX (more than 65279 sections) not supported");
    if (s.type == SHT_SYMTAB || s.type == SHT_RELA || s.type == SHT_GROUP) continue;
    if (s.type == SHT_STRTAB && (i == strtab || i == shstrndx)) continue;
    Section m; m.name = secname(s.name); m.type = s.type; m.flags = s.flags; m.addr = s.addr; m.addralign = s.addralign ? s.addralign : 1;
    m.entsize = s.entsize; m.info = s.info; m.size = s.size;
    if (s.type != SHT_NOBITS) { if (s.offset + s.size > bytes.size()) r.fail("section " + m.name + " beyond end of file"); m.data.assign(bytes.begin() + static_cast<long>(s.offset), bytes.begin() + static_cast<long>(s.offset + s.size)); }
    map[i] = static_cast<int>(o.sections.size()); o.sections.push_back(std::move(m));
  }
  // link fields (SHF_LINK_ORDER) after the map exists
  for (uint16_t i = 1; i < shnum; ++i) {
    if (map[i] < 0) continue;
    auto& m = o.sections[static_cast<size_t>(map[i])];
    if (m.flags & SHF_LINK_ORDER) { if (sh[i].link >= shnum || map[sh[i].link] < 0) r.fail("SHF_LINK_ORDER section " + m.name + " links to a dropped section"); m.link = map[sh[i].link]; }
  }
  // symbols
  const auto& st = sh[symtab];
  if (st.entsize != 24) r.fail("bad symtab entsize");
  size_t nsym = st.size / 24;
  for (size_t k = 0; k < nsym; ++k) {
    size_t off = st.offset + 24 * k;
    Symbol s; uint32_t nm = r.at<uint32_t>(off); uint8_t info = r.at<uint8_t>(off + 4); s.other = r.at<uint8_t>(off + 5);
    uint16_t shndx = r.at<uint16_t>(off + 6); s.value = r.at<uint64_t>(off + 8); s.size = r.at<uint64_t>(off + 16);
    s.bind = info >> 4; s.type = info & 0xf;
    s.name = nm ? r.str(sh[strtab].offset, sh[strtab].offset + sh[strtab].size, nm) : "";
    if (shndx == SHN_UNDEF) { s.shndx = -1; s.special = SHN_UNDEF; }
    else if (shndx >= SHN_LORESERVE) { if (shndx != SHN_ABS && shndx != SHN_COMMON) r.fail("unsupported special section index in symbol " + s.name); s.shndx = -1; s.special = shndx; }
    else { if (shndx >= shnum) r.fail("symbol section index out of range"); if (map[shndx] < 0) { if (s.type == STT_SECTION) { s.shndx = -1; s.special = SHN_UNDEF; s.name = "<dropped-section-symbol>"; } else r.fail("symbol " + s.name + " defined in a dropped section"); } else s.shndx = map[shndx]; }
    if (s.type == STT_SECTION && s.name.empty() && s.shndx >= 0) s.name = o.sections[static_cast<size_t>(s.shndx)].name;
    o.symbols.push_back(std::move(s));
  }
  // relocations and groups
  for (uint16_t i = 1; i < shnum; ++i) {
    const auto& s = sh[i];
    if (s.type == SHT_RELA) {
      if (s.entsize != 24) r.fail("bad rela entsize");
      if (s.info >= shnum || map[s.info] < 0) {
        // relocations for a dropped section (e.g. .rela.group? never) – only tolerate if the target is also dropped
        if (s.info < shnum && (sh[s.info].type == SHT_GROUP || sh[s.info].type == SHT_SYMTAB)) continue;
        r.fail("relocation section " + secname(s.name) + " targets a dropped section");
      }
      if (static_cast<int>(s.link) != symtab) r.fail("relocation section with foreign symbol table");
      auto& vec = o.relocs[map[s.info]];
      for (size_t k = 0; k < s.size / 24; ++k) {
        size_t off = s.offset + 24 * k; Rela e; e.offset = r.at<uint64_t>(off); uint64_t info = r.at<uint64_t>(off + 8);
        e.sym = static_cast<uint32_t>(info >> 32); e.type = static_cast<uint32_t>(info & 0xffffffffu); e.addend = r.at<int64_t>(off + 16);
        if (e.sym >= o.symbols.size()) r.fail("relocation symbol index out of range");
        vec.push_back(e);
      }
    } else if (s.type == SHT_GROUP) {
      Group g; g.name = secname(s.name); g.signature = s.info; if (g.signature >= o.symbols.size()) r.fail("group signature out of range");
      size_t n = s.size / 4; if (n == 0) r.fail("empty group");
      g.flags = r.at<uint32_t>(s.offset);
      for (size_t k = 1; k < n; ++k) {
        uint32_t m = r.at<uint32_t>(s.offset + 4 * k); if (m >= shnum) r.fail("group member index out of range");
        if (sh[m].type == SHT_RELA) continue;            // relocation sections of members are implied; the writer re-adds them
        if (map[m] < 0) r.fail("group member is a dropped section");
        g.members.push_back(map[m]);
      }
      o.groups.push_back(std::move(g));
    }
  }
  return o;
}

std::vector<uint8_t> write_object(const Object& o) {
  // Symbol ordering: locals first (stable), then the rest; build index remap.
  std::vector<uint32_t> order; order.reserve(o.symbols.size());
  for (uint32_t i = 0; i < o.symbols.size(); ++i) if (o.symbols[i].bind == STB_LOCAL) order.push_back(i);
  uint32_t nlocal = static_cast<uint32_t>(order.size());
  for (uint32_t i = 0; i < o.symbols.size(); ++i) if (o.symbols[i].bind != STB_LOCAL) order.push_back(i);
  if (o.symbols.empty() || o.symbols[0].bind != STB_LOCAL) throw std::runtime_error("symbol 0 must be the null local symbol");
  std::vector<uint32_t> remap(o.symbols.size());
  for (uint32_t k = 0; k < order.size(); ++k) remap[order[k]] = k;

  // Output section plan: [0]=null, groups first, then model sections (skipping index 0), then symtab, strtab, shstrtab, rela.*
  struct Out { std::string name; uint32_t type; uint64_t flags, addr, align, entsize; uint32_t link, info; std::vector<uint8_t> data; uint64_t nobits_size; };
  std::vector<Out> outs; outs.push_back({"", SHT_NULL, 0, 0, 0, 0, 0, 0, {}, 0});
  std::vector<int> secidx(o.sections.size(), -1);       // model index -> output index
  std::vector<int> groupidx(o.groups.size(), -1);
  for (size_t g = 0; g < o.groups.size(); ++g) { groupidx[g] = static_cast<int>(outs.size()); outs.push_back({o.groups[g].name.empty() ? ".group" : o.groups[g].name, SHT_GROUP, 0, 0, 4, 4, 0, 0, {}, 0}); }
  for (size_t i = 1; i < o.sections.size(); ++i) {
    const auto& s = o.sections[i]; secidx[i] = static_cast<int>(outs.size());
    outs.push_back({s.name, s.type, s.flags, s.addr, s.addralign ? s.addralign : 1, s.entsize, 0, s.info, s.data, s.type == SHT_NOBITS ? s.size : s.data.size()});
  }
  int symtab_i = static_cast<int>(outs.size()); outs.push_back({".symtab", SHT_SYMTAB, 0, 0, 8, 24, 0, nlocal, {}, 0});
  int strtab_i = static_cast<int>(outs.size()); outs.push_back({".strtab", SHT_STRTAB, 0, 0, 1, 0, 0, 0, {0}, 0});
  int shstr_i = static_cast<int>(outs.size()); outs.push_back({".shstrtab", SHT_STRTAB, 0, 0, 1, 0, 0, 0, {0}, 0});
  outs[static_cast<size_t>(symtab_i)].link = static_cast<uint32_t>(strtab_i);
  // link fields
  for (size_t i = 1; i < o.sections.size(); ++i) if (o.sections[i].link >= 0) outs[static_cast<size_t>(secidx[i])].link = static_cast<uint32_t>(secidx[static_cast<size_t>(o.sections[i].link)]);
  // string table + symbols
  auto& strtab = outs[static_cast<size_t>(strtab_i)].data;
  auto add_str = [&](const std::string& s) { if (s.empty()) return 0u; uint32_t off = static_cast<uint32_t>(strtab.size()); strtab.insert(strtab.end(), s.begin(), s.end()); strtab.push_back(0); return off; };
  auto& symdata = outs[static_cast<size_t>(symtab_i)].data;
  for (uint32_t k = 0; k < order.size(); ++k) {
    const Symbol& s = o.symbols[order[k]];
    uint32_t nm = (s.type == STT_SECTION) ? 0u : add_str(s.name);
    put(symdata, nm); put(symdata, static_cast<uint8_t>((s.bind << 4) | (s.type & 0xf))); put(symdata, s.other);
    uint16_t shndx;
    if (s.shndx >= 0) { if (secidx[static_cast<size_t>(s.shndx)] < 0) throw std::runtime_error("symbol " + s.name + " refers to an unknown section"); shndx = static_cast<uint16_t>(secidx[static_cast<size_t>(s.shndx)]); }
    else shndx = s.special;
    put(symdata, shndx); put(symdata, s.value); put(symdata, s.size);
  }
  // relocation sections (remember their output index per target for group membership)
  std::map<int, int> rela_of;
  for (const auto& [target, vec] : o.relocs) {
    if (vec.empty()) continue;
    if (target <= 0 || static_cast<size_t>(target) >= o.sections.size()) throw std::runtime_error("relocation target out of range");
    uint64_t rflags = SHF_INFO_LINK | ((o.sections[static_cast<size_t>(target)].flags & SHF_GROUP) ? SHF_GROUP : 0);
    Out rs{".rela" + o.sections[static_cast<size_t>(target)].name, SHT_RELA, rflags, 0, 8, 24, static_cast<uint32_t>(symtab_i), static_cast<uint32_t>(secidx[static_cast<size_t>(target)]), {}, 0};
    for (const Rela& e : vec) { put(rs.data, e.offset); put(rs.data, (static_cast<uint64_t>(remap[e.sym]) << 32) | e.type); put(rs.data, e.addend); }
    rela_of[target] = static_cast<int>(outs.size()); outs.push_back(std::move(rs));
  }
  // group contents: members plus their relocation sections (gABI requires them in the group)
  for (size_t g = 0; g < o.groups.size(); ++g) {
    auto& out = outs[static_cast<size_t>(groupidx[g])]; put(out.data, o.groups[g].flags);
    for (int m : o.groups[g].members) { put(out.data, static_cast<uint32_t>(secidx[static_cast<size_t>(m)])); if (auto it = rela_of.find(m); it != rela_of.end()) put(out.data, static_cast<uint32_t>(it->second)); }
    out.link = static_cast<uint32_t>(symtab_i); out.info = remap[o.groups[g].signature];
  }
  // section-name strings
  auto& shstr = outs[static_cast<size_t>(shstr_i)].data;
  std::vector<uint32_t> nameoff(outs.size(), 0);
  for (size_t i = 1; i < outs.size(); ++i) { nameoff[i] = static_cast<uint32_t>(shstr.size()); shstr.insert(shstr.end(), outs[i].name.begin(), outs[i].name.end()); shstr.push_back(0); }
  // layout
  std::vector<uint8_t> f(64, 0);
  std::vector<uint64_t> offs(outs.size(), 0);
  for (size_t i = 1; i < outs.size(); ++i) {
    align_to(f, outs[i].align > 1 ? outs[i].align : 1); offs[i] = f.size();
    if (outs[i].type != SHT_NOBITS) f.insert(f.end(), outs[i].data.begin(), outs[i].data.end());
  }
  align_to(f, 8); uint64_t shoff = f.size();
  for (size_t i = 0; i < outs.size(); ++i) {
    const auto& s = outs[i];
    put(f, nameoff[i]); put(f, s.type); put(f, s.flags); put(f, s.addr); put(f, i == 0 ? 0ull : offs[i]);
    put(f, i == 0 ? 0ull : (s.type == SHT_NOBITS ? s.nobits_size : static_cast<uint64_t>(s.data.size())));
    put(f, s.link); put(f, s.info); put(f, i == 0 ? 0ull : s.align); put(f, s.entsize);
  }
  // header
  std::memcpy(f.data(), "\x7f" "ELF", 4); f[4] = 2; f[5] = 1; f[6] = 1; f[7] = o.osabi;
  put_at(f, 16, 1, 2); put_at(f, 18, o.machine, 2); put_at(f, 20, 1, 4); put_at(f, 24, 0, 8); put_at(f, 32, 0, 8); put_at(f, 40, shoff, 8);
  put_at(f, 48, o.eflags, 4); put_at(f, 52, 64, 2); put_at(f, 54, 0, 2); put_at(f, 56, 0, 2); put_at(f, 58, 64, 2);
  put_at(f, 60, outs.size(), 2); put_at(f, 62, static_cast<uint64_t>(shstr_i), 2);
  return f;
}

void write_object_file(const Object& o, const std::string& path) {
  auto bytes = write_object(o);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) throw std::runtime_error("cannot write " + path);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace opticc::elf
