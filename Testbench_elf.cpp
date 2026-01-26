// --- Standard C++ Headers ---
#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>

// --- Vitis HLS Headers ---
#include <ap_int.h>

// --- Local Project Headers ---
#include "elf.h"
#include "elfFile.h"
#include "core.h"

// ============================================================================
//  USER CONFIGURATION SWITCHES
// ============================================================================

// 1. Execution Limit
#define INSTRUCTION_LIMIT 1000000 

// 2. Debug Switches
const bool ENABLE_CORE_DEBUG = false;
const bool ENABLE_MEMORY_INSPECTION = false; 

// ============================================================================

// UNIFIED RAM ARRAY
ap_uint<32> ram[RAM_SIZE];

extern void riscv_init();
// UPDATED SIGNATURE
extern void riscv_step(volatile uint32_t* ram, int cycles);

int main(int argc, char* argv[])
{
    const char* elf_filename = "rsort.riscv";
    if (argc > 1) elf_filename = argv[1];

    std::cout << "[TESTBENCH] Loading ELF: " << elf_filename << "\n";
    
    memset(ram, 0, sizeof(ram));

    // Use the shared class from elfFile.h
    ElfFile loader(elf_filename);
    ENTRY_PC = loader.load_to_mem(ram, RAM_SIZE);

    // DYNAMIC TOHOST CALCULATION
    unsigned tohost_idx = 0;
    if (loader.tohost_addr_found != 0) {
        tohost_idx = (loader.tohost_addr_found - DRAM_BASE) >> 2;
        std::cout << "[TESTBENCH] Detected .tohost at 0x" << std::hex << loader.tohost_addr_found 
                  << " (Index " << std::dec << tohost_idx << ")\n";
    } else {
        std::cout << "[TESTBENCH] WARNING: .tohost section not found, using default 0x80001000\n";
        tohost_idx = (0x80001000 - DRAM_BASE) >> 2;
    }

    std::cout << "\n[TESTBENCH] Initializing Core...\n";
    
    CORE_DEBUG = ENABLE_CORE_DEBUG; 
    
    riscv_init();

    std::cout << "\n[TESTBENCH] Starting Simulation (Max " << INSTRUCTION_LIMIT << " cycles)...\n";
    
    bool passed = false;

    // Single Call to Hardware
    // The hardware will loop internally until it hits the ecall or the limit
    riscv_step((volatile uint32_t*)ram, INSTRUCTION_LIMIT);

    // Check results after hardware returns
    uint32_t tohost = ram[tohost_idx];

    if (tohost & 1) { 
        int exit_code = tohost >> 1;
        if (exit_code == 0) {
            std::cout << "[TESTBENCH] PASS (Hardware exited via ecall)\n";
            passed = true;
        } else {
            std::cout << "[TESTBENCH] FAIL (Code: " << exit_code << ")\n";
            passed = false;
        }
    } else {
        // If tohost is 0, it means we hit the cycle limit without finishing
        std::cout << "[TESTBENCH] TIMEOUT (Reached " << INSTRUCTION_LIMIT << " cycles without ecall)\n";
        passed = false;
    }

    // ================================================================
    // MEMORY INSPECTION (Using Configuration Switch)
    // Note: data_addr is specific to 'rsort.riscv'. Update for other files (This is disabled by default).
    // ================================================================
    if (passed && ENABLE_MEMORY_INSPECTION) {
        std::cout << "\n[INSPECTION] Checking 'input_data' array in memory...\n";
        
        // Force the address of <input_data> from your dump file
        uint32_t data_addr = 0x80005d94; 

        if (data_addr != 0) {
            // Convert to RAM Index: (Address - Base) / 4
            unsigned ram_idx = (data_addr - DRAM_BASE) >> 2;
            
            std::cout << "Reading from 0x" << std::hex << data_addr << std::dec << " (RAM Index " << ram_idx << ")\n";
            std::cout << "These values MUST be sorted (Low -> High):\n";
            std::cout << "----------------------------------------\n";
            
            for (int k = 0; k < 20; k++) {
                if (ram_idx + k < RAM_SIZE) {
                    int value = (int)ram[ram_idx + k];
                    std::cout << "[" << k << "] " << value << "\n";
                }
            }
            std::cout << "----------------------------------------\n";
        }
    }

    return passed ? 0 : 1;
}