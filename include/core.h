#ifndef CORE_H
#define CORE_H

#include <ap_int.h>
#include <iostream>

// =======================================================
// Global memory configuration
// =======================================================
#define RAM_SIZE 33554432 

// RISC-V Default Memory Map
#define DRAM_BASE 0x80000000
#define DMEM_STACK_TOP 0x87FFFFFF

// =======================================================
// Global ELF / memory configuration variables
// =======================================================
extern ap_uint<32> ENTRY_PC;
extern ap_uint<32> DTB_ADDR;
extern bool CORE_DEBUG;

// =======================================================
// Core Interface
// =======================================================

// Initialization function
void riscv_init();

// Unified Memory Step Function
// [UPDATED] 'cycles_output' is now a pointer so the core can write back the final count.
void riscv_step(volatile uint32_t* ram, int* cycles_output);

#endif // CORE_H