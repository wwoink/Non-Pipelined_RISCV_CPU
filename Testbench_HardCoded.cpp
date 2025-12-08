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

// 1. Hardcoded Path
#define ELF_PATH "I:/Vitis_Files/Pipeline_Tests/Global_Core_Revised/Benchmarks/rv32ui-p-benchmarks/rsort.riscv"

// 2. Execution Limit (Total Cycles)
#define INSTRUCTION_LIMIT 500000 

// 3. Batch Size (New: How many cycles to run per hardware call)
#define BATCH_SIZE 5000

// 4. Debug Switches
const bool ENABLE_CORE_DEBUG = false;
const bool ENABLE_MEMORY_INSPECTION = false; 

// ============================================================================

// UNIFIED RAM ARRAY
ap_uint<32> ram[RAM_SIZE];

extern void riscv_init();
// UPDATED SIGNATURE: Now accepts the cycle count
extern void riscv_step(volatile uint32_t* ram, int cycles);

int main(int argc, char* argv[])
{
    const char* elf_filename = ELF_PATH;

    std::cout << "[TESTBENCH] Loading ELF: " << elf_filename << "\n";
    
    // Clear Memory
    memset(ram, 0, sizeof(ram));

    // Load ELF
    ElfFile loader(elf_filename);
    ENTRY_PC = loader.load_to_mem(ram, RAM_SIZE);

    if (ENTRY_PC == 0) {
        std::cout << "[TESTBENCH] CRITICAL ERROR: Could not load ELF (ENTRY_PC is 0).\n";
        return 1; 
    }

    // Dynamic Tohost Calculation
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

    std::cout << "\n[TESTBENCH] Starting Batch Simulation (" << BATCH_SIZE << " cycles/step)...\n";
    
    bool passed = false;
    bool done = false;

    // UPDATED LOOP: Steps by BATCH_SIZE
    for (int i = 0; i < INSTRUCTION_LIMIT; i += BATCH_SIZE) {
        
        // --- RUN BATCH ---
        // This runs 5000 cycles in hardware before returning
        riscv_step((volatile uint32_t*)ram, BATCH_SIZE);

        uint32_t tohost = ram[tohost_idx];

        if (tohost != 0) {
            ram[tohost_idx] = 0; // ACK writes

            // Handle Syscalls (LSB 0)
            if ((tohost & 1) == 0) {
                unsigned fromhost_idx = tohost_idx + 16;
                if (fromhost_idx < RAM_SIZE) {
                    ram[fromhost_idx] = 1; 
                }
            }
            
            // Handle Exit (LSB 1)
            if (tohost & 1) { 
                int exit_code = tohost >> 1;
                if (exit_code == 0) {
                    // Note: 'i' is the start of the batch, so we say "approx"
                    std::cout << "[TESTBENCH] PASS at approx cycle " << i << "\n";
                    passed = true;
                } else {
                    std::cout << "[TESTBENCH] FAIL (Code: " << exit_code << ") at approx cycle " << i << "\n";
                    passed = false;
                }
                done = true;
                break;
            }
        }
    }

    if (!done) {
        std::cout << "[TESTBENCH] TIMEOUT (Reached " << INSTRUCTION_LIMIT << " cycles)\n";
        passed = false;
    }

    // ================================================================
    // MEMORY INSPECTION 
    // ================================================================
    if (passed && ENABLE_MEMORY_INSPECTION) {
        std::cout << "\n[INSPECTION] Checking 'input_data' array in memory...\n";
        uint32_t data_addr = 0x80005d94; 
        
        if (data_addr != 0) {
            unsigned ram_idx = (data_addr - DRAM_BASE) >> 2;
            std::cout << "Reading from 0x" << std::hex << data_addr << std::dec << "\n";
            for (int k = 0; k < 20; k++) {
                if (ram_idx + k < RAM_SIZE) {
                    int value = (int)ram[ram_idx + k];
                    std::cout << "[" << k << "] " << value << "\n";
                }
            }
        }
    }

    // FINAL VITIS STATUS CHECK
    if (passed) {
        return 0; // SUCCESS 
    } else {
        return 1; // FAILURE 
    }
}