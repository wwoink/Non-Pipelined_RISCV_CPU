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

// 1. Execution Limit (Prevent infinite loops)
#define INSTRUCTION_LIMIT 10000000 

// 2. Instruction Trace: Set TRUE to print every instruction (Opcode, Regs, ALU)
const bool ENABLE_CORE_DEBUG = false;

// 3. Data Verification: Set TRUE to print the data array after success
const bool ENABLE_MEMORY_INSPECTION = false; 

// ============================================================================

// UNIFIED RAM ARRAY
ap_uint<32> ram[RAM_SIZE];

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
    
    // APPLY USER CONFIGURATION
    CORE_DEBUG = ENABLE_CORE_DEBUG; 
    
    riscv_init();

    std::cout << "\n[TESTBENCH] Starting simulation loop...\n";
    
    bool passed = false;
    bool done = false;

    for (int i = 0; i < INSTRUCTION_LIMIT; i++) {
        
        riscv_step((volatile uint32_t*)ram);

        uint32_t tohost_val = ram[tohost_idx];

        if (tohost_val != 0) {
            ram[tohost_idx] = 0; // ACK writes
            
            // Check LSB for Exit Code
            if (tohost_val & 1) { 
                int exit_code = tohost_val >> 1;
                if (exit_code == 0) {
                    std::cout << "[TESTBENCH] PASS at cycle " << i << "\n";
                    passed = true;
                } else {
                    std::cout << "[TESTBENCH] FAIL (Code: " << exit_code << ") at cycle " << i << "\n";
                }
                done = true;
                break;
            }
        }
    }

    if (!done) std::cout << "[TESTBENCH] TIMEOUT\n";

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

    return 0;
}