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

// --- OS Specific Headers (Batch Runner Only) ---
#include <windows.h> // Required for directory scanning (FindFirstFile)

// --- Configuration ---
// Increase if needed.
#define TEST_TIMEOUT   5000000 

// --- Unified Memory Array ---
ap_uint<32> ram[RAM_SIZE]; 

extern void riscv_init();
extern void riscv_step(volatile uint32_t* ram); 

// ============================================================================
// Directory Scanner (Windows)
// ============================================================================
std::vector<std::string> get_test_files(std::string folder) {
    std::vector<std::string> files;
    std::string search_path = folder + "/*";
    WIN32_FIND_DATA fd;
    HANDLE hFind = ::FindFirstFile(search_path.c_str(), &fd);
    
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::string filename = fd.cFileName;
                // Filter: Ignore .dump files, hidden files, and non-elf files
                if (filename.find(".dump") == std::string::npos && filename.find(".") != 0) {
                    files.push_back(filename);
                }
            }
        } while (::FindNextFile(hFind, &fd));
        ::FindClose(hFind);
    }
    return files;
}

// ============================================================================
// Main Batch Loop
// ============================================================================
int main(int argc, char* argv[])
{
    std::string folder_path = "";
    if (argc > 1) folder_path = argv[1];
    else {
        std::cout << "Usage: <executable> <path_to_test_folder>\n";
        return 1;
    }

    std::vector<std::string> tests = get_test_files(folder_path);
    std::cout << "[BATCH] Found " << tests.size() << " potential tests in " << folder_path << "\n";
    
    if (tests.empty()) return 1;

    int total_pass = 0;
    int total_fail = 0;
    
    std::cout << "\n==================================================================\n";
    std::cout << "  RISC-V REGRESSION RUNNER  \n";
    std::cout << "==================================================================\n";

    for (const auto& test_name : tests) {
        std::string full_path = folder_path + "/" + test_name;
        
        // 1. Clear Memory
        memset(ram, 0, sizeof(ram));

        // 2. Load ELF (Using shared class from elfFile.h)
        ElfFile loader(full_path.c_str());
        ENTRY_PC = loader.load_to_mem(ram, RAM_SIZE);

        if (ENTRY_PC == 0) continue; // Skip invalid files

        // 3. Dynamic Tohost Calculation
        unsigned tohost_idx = 0;
        if (loader.tohost_addr_found != 0) {
            tohost_idx = (loader.tohost_addr_found - DRAM_BASE) >> 2;
        } else {
            tohost_idx = (0x80001000 - DRAM_BASE) >> 2;
        }

        // 4. Init Core
        CORE_DEBUG = false; // Keep logs clean
        riscv_init();

        // 5. Run Simulation
        bool finished = false;
        for (int i = 0; i < TEST_TIMEOUT; i++) {
            
            // Step Core
            riscv_step((volatile uint32_t*)ram);
            
            // Check Host Interface
            uint32_t tohost = ram[tohost_idx];
            
            if (tohost != 0) {
                // ACK the write by clearing memory (Important for benchmarks)
                ram[tohost_idx] = 0;

                // Logic: LSB 1 = Exit, LSB 0 = Syscall
                if (tohost & 1) {
                    int exit_code = tohost >> 1;
                    if (exit_code == 0) {
                        std::cout << std::left << std::setw(30) << test_name << " : PASS\n";
                        total_pass++;
                    } else {
                        std::cout << std::left << std::setw(30) << test_name << " : FAIL (Code: " << exit_code << ")\n";
                        total_fail++;
                    }
                    finished = true;
                    break;
                } else {
                    // It's a Syscall (e.g., printf char). 
                    // In this batch runner, we ignore output to keep the console clean.
                    // But clearing the memory above ensures the core doesn't hang waiting for ACK.
                }
            }
        }

        if (!finished) {
            std::cout << std::left << std::setw(30) << test_name << " : TIMEOUT\n";
            total_fail++;
        }
    }

    std::cout << "\n==================================================================\n";
    std::cout << "SUMMARY: " << total_pass << " PASSED, " << total_fail << " FAILED\n";
    std::cout << "==================================================================\n";

    return 0;
}