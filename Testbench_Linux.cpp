#include <iostream>
#include <fstream>
#include <vector>
#include "core.h" // Ensures we see the global variables ENTRY_PC and DTB_ADDR

// --------------------------------------------------------------------------
// SIMULATED RAM SETUP
// --------------------------------------------------------------------------
// 128MB = 33,554,432 words
#define RAM_SIZE_BYTES (128 * 1024 * 1024) 
#define RAM_SIZE_WORDS (RAM_SIZE_BYTES / 4)

// We use ap_uint<32> for the array, but we will cast it when calling the core
ap_uint<32> ram[RAM_SIZE_WORDS];

// --------------------------------------------------------------------------
// HELPER: Load Raw Binary File to RAM
// --------------------------------------------------------------------------
bool load_binary_to_ram(const char* filename, uint32_t start_addr) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[ERROR] Could not open file: " << filename << std::endl;
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size <= 0) {
        std::cerr << "[ERROR] File is empty: " << filename << std::endl;
        return false;
    }

    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size)) {
        std::cerr << "[ERROR] Failed to read file data: " << filename << std::endl;
        return false;
    }

    // Calculate start index (Assumes RAM Base is 0x80000000)
    if (start_addr < 0x80000000) {
        std::cerr << "[ERROR] Address 0x" << std::hex << start_addr 
                  << " is below DRAM base!" << std::endl;
        return false;
    }
    
    uint32_t ram_start_idx = (start_addr - 0x80000000) / 4;

    for (int i = 0; i < size; i++) {
        uint32_t word_idx = ram_start_idx + (i / 4);
        int byte_shift = (i % 4) * 8; // Little Endian (0, 8, 16, 24)

        if (word_idx >= RAM_SIZE_WORDS) {
            std::cerr << "[ERROR] File loading overflowed RAM!" << std::endl;
            return false;
        }

        // Read-Modify-Write the word
        uint32_t current_word = (unsigned int)ram[word_idx];
        current_word &= ~(0xFF << byte_shift);             // Clear byte
        current_word |= ((uint8_t)buffer[i] << byte_shift); // Set byte
        ram[word_idx] = current_word;
    }

    std::cout << "[LOADER] Loaded " << filename << " to 0x" << std::hex << start_addr 
              << " (" << std::dec << size << " bytes)" << std::endl;
    return true;
}

// --------------------------------------------------------------------------
// MAIN TESTBENCH
// --------------------------------------------------------------------------
int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    // 1. PATHS
    CORE_DEBUG = false;
    const char* KERNEL_PATH = "I:/Vitis_Files/Pipeline_Tests/Global_Core_Revised/Image";
    const char* DTB_PATH    = "I:/Vitis_Files/Pipeline_Tests/Global_Core_Revised/system.dtb";
    
    std::cout << "--------------------------------------------------\n";
    std::cout << "      RISC-V LINUX BOOT SIMULATION                \n";
    std::cout << "--------------------------------------------------\n";

    // 2. Clear RAM
    for(int i=0; i<RAM_SIZE_WORDS; i++) ram[i] = 0;

    // 3. Load Files
    // Kernel to start of RAM (0x80000000)
    if (!load_binary_to_ram(KERNEL_PATH, 0x80000000)) return -1;
    
    // DTB to 16MB offset (0x81000000)
    uint32_t dtb_load_addr = 0x80800000;
    if (!load_binary_to_ram(DTB_PATH, dtb_load_addr)) return -1;

    // 4. Configure Global Variables (Communication with Core)
    ENTRY_PC = 0x80000000;    // Kernel Entry Point
    DTB_ADDR = dtb_load_addr; // Device Tree Pointer
    
    // Check if DTB loaded correctly
    uint32_t dtb_magic = (unsigned int)ram[(0x80800000 - 0x80000000)/4];
    // Swap bytes if needed (DTB is Big Endian, RAM is Little Endian usually)
    // Just printing it is enough to see if it's non-zero.
    std::cout << "[DEBUG] DTB First Word at 0x80800000: 0x" << std::hex << dtb_magic << std::endl;

    std::cout << "[INIT] ENTRY_PC set to: 0x" << std::hex << ENTRY_PC << "\n";
    std::cout << "[INIT] DTB_ADDR set to: 0x" << std::hex << DTB_ADDR << "\n";

    // 5. Initialize Core
    riscv_init(); 

    std::cout << "[RUN] Starting Execution loop..." << std::endl;
    
    // 6. Execution Loop
    int step_count = 0;
    int core_cycles = 0; // Dummy variable to catch the cycle count output

    riscv_step((volatile uint32_t*)ram, 10000000, &core_cycles);

    return 0;
}