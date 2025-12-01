#ifndef __ELFFILE
#define __ELFFILE

#include <cstdio>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <ap_int.h>
#include "elf.h"

static constexpr uint8_t ELF_MAGIC[] = {ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3};

static constexpr size_t E_SHOFF     = 0x20;
static constexpr size_t E_SHENTSIZE = 0x2E;
static constexpr size_t E_SHNUM     = 0x30;
static constexpr size_t E_SHSTRNDX  = 0x32;
static constexpr size_t E_PHOFF     = 0x1C; // Program Header Offset
static constexpr size_t E_PHNUM     = 0x2C; // Number of Program Headers

template<unsigned N> constexpr size_t little_endian(const uint8_t *bytes){
    return (bytes[N-1] << 8 * (N-1)) | little_endian<N-1>(bytes);
}

template<> constexpr size_t little_endian<0>(const uint8_t *bytes){
    return 0;
}

template<unsigned N> constexpr size_t big_endian(const unsigned char *bytes){
    return (bytes[0] << 8 * (N-1)) | big_endian<N-1>(bytes+1);
}

template<> constexpr size_t big_endian<0>(const unsigned char *bytes){
    return 0;
}

template <typename T> const T find_by_name(const std::vector<T> v, const std::string name)
{
  for(const auto &s : v){
      if (s.name == name)
        return s;
  }
  fprintf(stderr, "Error: \"%s\" name not found\n", name.c_str());
  exit(-1);
}

struct ElfSection {
  unsigned int size;
  unsigned int offset;
  unsigned int nameIndex;
  unsigned int address;
  unsigned int type;
  unsigned int info;

  std::string name;

  template <typename ElfShdr> ElfSection(const ElfShdr);
};

struct ElfSymbol {
  unsigned int nameIndex;
  unsigned int type;
  unsigned int offset;
  unsigned int size;
  unsigned int section;
  unsigned int value;

  std::string name;

  template <typename ElfSymT> ElfSymbol(const ElfSymT);
};

class ElfFile {
public:
  std::vector<ElfSection> sectionTable;
  std::vector<ElfSymbol> symbols;
  std::vector<uint8_t> content;
  
  // Track where the tohost section is located
  uint32_t tohost_addr_found = 0;

  ElfFile(const char* pathToElfFile);
  ~ElfFile() = default;
  
  // Unified Memory Load
  ap_uint<32> load_to_mem(ap_uint<32> ram[], int ram_depth);

  // Helper to find data symbols for verification
  uint32_t get_symbol_addr(std::string name) {
      for(auto& s : symbols) {
          if (s.name == name) return s.value;
      }
      return 0;
  }

private:
  template <typename ElfSymT> void readSymbolTable();
  template <typename ElfShdrT> void fillSectionTable();

  void fillNameTable();
  void fillSymbolsName();
};

// --- Implementations ---

// Constructor
inline ElfFile::ElfFile(const char* pathToElfFile) {
    std::ifstream elfFile(pathToElfFile, std::ios::binary); // Now works because <fstream> is included
    if (!elfFile) {
        fprintf(stderr, "Error: cannot open file %s\n", pathToElfFile);
        exit(-1);
    }
    content.assign(std::istreambuf_iterator<char>(elfFile), {});
    
    if (!std::equal(std::begin(ELF_MAGIC), std::end(ELF_MAGIC), content.begin())) {
        fprintf(stderr, "Error: Not a valid ELF file\n");
        exit(-1);
    }
    
    fillSectionTable<Elf32_Shdr>();
    fillNameTable();
    readSymbolTable<Elf32_Sym>();
    fillSymbolsName();
}

template <typename ElfSymT> void ElfFile::readSymbolTable() {
  for (const auto& section : sectionTable) {
    if (section.type == SHT_SYMTAB) {
      const auto* rawSymbols = reinterpret_cast<ElfSymT*>(&content[section.offset]);
      const auto N           = section.size / sizeof(ElfSymT);
      for (int i = 0; i < N; i++)
        symbols.push_back(ElfSymbol(rawSymbols[i]));
    }
  }
}

template <typename ElfShdrT> void ElfFile::fillSectionTable() {
  const auto tableOffset  = little_endian<4>(&content[E_SHOFF]);
  const auto tableSize    = little_endian<2>(&content[E_SHNUM]);
  const auto* rawSections = reinterpret_cast<ElfShdrT*>(&content[tableOffset]);

  sectionTable.reserve(tableSize);
  for (int i = 0; i < tableSize; i++)
    sectionTable.push_back(ElfSection(rawSections[i]));
}

template <typename ElfShdrT> ElfSection::ElfSection(const ElfShdrT header) {
  offset    = (header.sh_offset);
  size      = (header.sh_size);
  nameIndex = (header.sh_name);
  address   = (header.sh_addr);
  type      = (header.sh_type);
  info      = (header.sh_info);
}

template <typename ElfSymT> ElfSymbol::ElfSymbol(const ElfSymT sym) {
  offset    = sym.st_value;
  type      = ELF32_ST_TYPE(sym.st_info);
  section   = sym.st_shndx;
  size      = sym.st_size;
  nameIndex = sym.st_name;
}

inline void ElfFile::fillNameTable() {
    const auto nameTableIndex = little_endian<2>(&content[E_SHSTRNDX]);
    const auto nameTableOffset = sectionTable[nameTableIndex].offset;
    const char* names = reinterpret_cast<const char*>(&content[nameTableOffset]);
    for (auto& section : sectionTable) section.name = std::string(&names[section.nameIndex]);
}

inline void ElfFile::fillSymbolsName() {
    const auto sec = find_by_name(sectionTable, ".strtab");
    auto names = reinterpret_cast<const char*>(&content[sec.offset]);
    for (auto& symbol : symbols) symbol.name = std::string(&names[symbol.nameIndex]);
}

// Unified Load Logic
/*inline ap_uint<32> ElfFile::load_to_mem(ap_uint<32> ram_ptr[], int ram_depth) {
    const auto entry_pc = little_endian<4>(&content[0x18]);
    uint32_t DRAM_BASE_ADDR = 0x80000000;

    for (auto& sec : sectionTable) {
        bool is_loadable = (sec.name == ".text" || sec.name == ".text.init" || sec.name == ".data" || 
                            sec.name == ".tohost" || sec.name == ".rodata" || sec.name == ".sdata" || 
                            sec.name == ".sbss" || sec.name == ".bss" || sec.name == ".text.startup");
        
        if (is_loadable) {
            if (sec.name == ".tohost") this->tohost_addr_found = sec.address;

            uint32_t phys_addr = sec.address & 0x000FFFFF; 
            size_t start = (phys_addr - (DRAM_BASE_ADDR & 0x000FFFFF)) >> 2;
            size_t words = (sec.size + 3) / 4;

            if (start < (size_t)ram_depth) {
                if (sec.type == SHT_NOBITS) { // BSS (Zero init)
                    memset(&ram_ptr[start], 0, words * 4);
                } else {
                    memcpy(&ram_ptr[start], &content[sec.offset], sec.size);
                    //std::cout << "[ELF] Loaded " << sec.name << " to RAM idx " << start << "\n";
                }
            }
        }
    }
    //std::cout << "[ELF] Entry PC = 0x" << std::hex << entry_pc << std::dec << "\n";
    return (ap_uint<32>)entry_pc & 0xFFFFFFFF;
}*/
// Unified Load Logic (Segment-Based Loading)
inline ap_uint<32> ElfFile::load_to_mem(ap_uint<32> ram_ptr[], int ram_depth) {
    const auto entry_pc = little_endian<4>(&content[0x18]);
    uint32_t DRAM_BASE_ADDR = 0x80000000;

    // ---------------------------------------------------------
    // STEP 1: Scan Sections ONLY to find .tohost address
    // ---------------------------------------------------------
    this->tohost_addr_found = 0;
    for (const auto& sec : sectionTable) {
        if (sec.name == ".tohost") {
            this->tohost_addr_found = sec.address;
            // std::cout << "[ELF] Found .tohost at 0x" << std::hex << sec.address << "\n";
        }
    }

    // ---------------------------------------------------------
    // STEP 2: Load Data using Program Headers (Segments)
    // ---------------------------------------------------------
    const auto ph_off = little_endian<4>(&content[E_PHOFF]);
    const auto ph_num = little_endian<2>(&content[E_PHNUM]);
    
    const auto* ph_table = reinterpret_cast<const Elf32_Phdr*>(&content[ph_off]);

    for (int i = 0; i < ph_num; i++) {
        const Elf32_Phdr& ph = ph_table[i];

        // We only care about Loadable Segments with a non-zero memory size
        if (ph.p_type == PT_LOAD && ph.p_memsz > 0) {
            
            // Calculate RAM Index
            uint32_t phys_addr = ph.p_paddr; 
            
            // Safety check: Ensure address is within DRAM range
            if (phys_addr < DRAM_BASE_ADDR) continue;

            size_t start_idx = (phys_addr - DRAM_BASE_ADDR) >> 2;
            
            // Bounds check
            if (start_idx >= (size_t)ram_depth) {
                std::cerr << "[ELF] Warning: Segment 0x" << std::hex << phys_addr 
                          << " is outside RAM bounds.\n";
                continue;
            }

            // 1. Copy File Data (Code/Data/Rodata)
            if (ph.p_filesz > 0) {
                // memcpy writes bytes; ram_ptr is 32-bit words. 
                // &ram_ptr[start_idx] gives the byte address of that word.
                memcpy(&ram_ptr[start_idx], &content[ph.p_offset], ph.p_filesz);
            }

            // 2. Handle BSS (Zero-init remaining memory)
            // If MemSize > FileSize, the rest is BSS and must be zeroed.
            if (ph.p_memsz > ph.p_filesz) {
                size_t bss_size = ph.p_memsz - ph.p_filesz;
                // Calculate byte offset where BSS starts
                uint8_t* ram_byte_ptr = (uint8_t*)ram_ptr;
                size_t bss_start_byte_idx = (start_idx * 4) + ph.p_filesz;
                
                memset(&ram_byte_ptr[bss_start_byte_idx], 0, bss_size);
            }

            /* Debug Output
            std::cout << "[ELF] Loaded Segment: Phys=0x" << std::hex << phys_addr
                      << " FileSz=0x" << ph.p_filesz 
                      << " MemSz=0x" << ph.p_memsz << "\n";
            */
        }
    }

    return (ap_uint<32>)entry_pc;
}

#endif