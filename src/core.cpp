#include <ap_int.h>
#include <iostream>
#include <cstring>
#include "core.h" 
#include <cstdio>

// ------------------------------------------------------------
// Global Debug Switch
// ------------------------------------------------------------
bool CORE_DEBUG = true; // This must be toggled in the testbench

// ------------------------------------------------------------  
// Global Configuration Switch
// ------------------------------------------------------------
// If false, the M-extension logic is completely pruned during synthesis.
const bool ENABLE_M_EXTENSION = true;
const bool ENABLE_A_EXTENSION = true; // Toggle for Atomics

// ------------------------------------------------------------
// Global Architectural State
// ------------------------------------------------------------
#ifdef __SYNTHESIS__
    // This value is baked into the FPGA bitstream as the Power-On default
    ap_uint<32> pc = 0x80000000; 
#else
    // This value is used during C Simulation (and later overwritten by riscv_init)
    ap_uint<32> pc = 0;          
#endif
ap_int<32>  regfile[32];
bool is_finished = false;

// --- Load Reserved / Store Conditional State ---
ap_uint<32> lr_addr = 0;
bool lr_valid = false;

// ------------------------------------------------------------
// Global pipeline registers
// ------------------------------------------------------------
ap_uint<32> IF_ID_instr = 0;
ap_uint<32> IF_ID_pc = 0;

ap_uint<7>  ID_EX_opcode = 0;
ap_uint<5>  ID_EX_rd = 0;
ap_uint<3>  ID_EX_funct3 = 0;
ap_uint<5>  ID_EX_rs1 = 0;
ap_uint<5>  ID_EX_rs2 = 0;
ap_uint<7>  ID_EX_funct7 = 0;
ap_int<32>  ID_EX_imm = 0;
ap_int<32>  ID_EX_pc = 0;
ap_int<32>  ID_EX_rs1_val = 0; 
ap_int<32>  ID_EX_rs2_val = 0; 
ap_uint<32> ID_EX_instr = 0; 

ap_int<32>  EX_MEM_alu_result = 0;
ap_uint<5>  EX_MEM_rd = 0;
bool        EX_MEM_mem_read = 0;
bool        EX_MEM_mem_write = 0;
bool        EX_MEM_reg_write = 0;
ap_uint<5>  EX_MEM_rs2 = 0;
ap_uint<3>  EX_MEM_funct3 = 0;
ap_int<32>  EX_MEM_store_val = 0;
bool        EX_MEM_is_trap = false; 

// --- New Pipeline Regs for Atomics ---
bool        EX_MEM_is_atomic = false;
ap_uint<5>  EX_MEM_atomic_op = 0;

ap_int<32>  MEM_WB_value = 0;
ap_uint<5>  MEM_WB_rd = 0;
bool        MEM_WB_reg_write = 0;
bool        MEM_WB_is_trap = false; 

bool        took_branch_or_jump = false;
ap_uint<32> next_pc               = 0;

// --- CSRs ---
ap_uint<32> csr_mtvec = 0;
ap_uint<32> csr_mepc = 0;
ap_uint<32> csr_mcause = 0;
ap_uint<32> csr_mscratch = 0;

ap_uint<32> csr_mcycle = 0;   // Cycle Counter (0xB00)
ap_uint<32> csr_minstret = 0; // Instructions Retired (0xB02)
ap_uint<32> csr_mstatus = 0;  // Status Register (0x300)

// --- Interrupts & Timer ---
ap_uint<32> csr_mie = 0; // Interrupt Enable Register (0x304)
ap_uint<32> csr_mip = 0; // Interrupt Pending Register (0x344)
// The CLINT (Core Local Interruptor) requires a 64-bit timer comparison
ap_uint<64> mtimecmp = 0xFFFFFFFFFFFFFFFF;

// --- Global Variable Master Definitions ---
ap_uint<32> ENTRY_PC;
ap_uint<32> DTB_ADDR;

// ------------------------------------------------------------
// Initialization Function
// ------------------------------------------------------------
void riscv_init() {
    pc = ENTRY_PC;
    for(int i=0; i<32; i++) regfile[i] = 0;

    regfile[1] = (ap_int<32>)0xDEADBEEF;       
    regfile[0] = 0;                           
    regfile[2] = (ap_int<32>) (unsigned)DMEM_STACK_TOP;

    //Linux setup
    regfile[10] = 0;         // a0 = Hart ID (0)
    regfile[11] = 0x80800000; // a1 = Device Tree Address
    
    if(CORE_DEBUG) {
        std::cout << "[INIT] Core Reset. PC=0x" << std::hex << (unsigned)pc 
                  << ", SP=0x" << (unsigned)regfile[2] << std::dec << std::endl;
    }

    IF_ID_instr = 0;
    took_branch_or_jump = false;
    EX_MEM_is_trap = false;
    MEM_WB_is_trap = false;
    is_finished = false; // Reset the finished flag

    csr_mcycle = 0;
    csr_minstret = 0;
    csr_mstatus = 0;
    
    // Reset Timer state
    csr_mie = 0;
    csr_mip = 0;
    mtimecmp = 0xFFFFFFFFFFFFFFFF;

    // Reset Atomic State
    lr_valid = false;
    lr_addr = 0;
}

// ------------------------------------------------------------
// Helper Functions: Immediate Extractors
// ------------------------------------------------------------
ap_int<32> sextI(ap_uint<32> insn) {
    #pragma HLS INLINE
    ap_int<12> imm12 = (ap_int<12>)(insn >> 20);
    return (ap_int<32>)imm12;
}

ap_int<32> sextS(ap_uint<32> insn) {
    #pragma HLS INLINE
    ap_uint<7> hi = insn.range(31,25);
    ap_uint<5> lo = insn.range(11,7);
    ap_uint<12> u = ((ap_uint<12>)hi << 5) | (ap_uint<12>)lo;
    ap_int<12> s = (ap_int<12>)u;
    return (ap_int<32>)s;
}

ap_int<32> sextB(ap_uint<32> insn) {
    #pragma HLS INLINE
    ap_uint<13> imm;
    imm[12]       = insn[31];       
    imm[11]       = insn[7];        
    imm.range(10,5) = insn.range(30,25); 
    imm.range(4,1)  = insn.range(11,8);  
    imm[0]          = 0;            
    return (ap_int<32>)((ap_int<13>)imm);
}

ap_int<32> sextJ(ap_uint<32> insn) {
    #pragma HLS INLINE
    ap_uint<21> u = ((ap_uint<21>)insn[31]        << 20) |
                    ((ap_uint<21>)insn.range(30,21) << 1 ) |
                    ((ap_uint<21>)insn[20]        << 11) |
                    ((ap_uint<21>)insn.range(19,12) << 12);
    ap_int<21> s = (ap_int<21>)u;
    return (ap_int<32>)s;
}

// ------------------------------------------------------------
// Pipeline Stages
// ------------------------------------------------------------

// Returns the fetched instruction
ap_uint<32> fetch(volatile uint32_t* ram) {
    #pragma HLS INLINE
    uint32_t phys_pc = pc & 0x07FFFFFF; 
    uint32_t im_idx = (phys_pc - (DRAM_BASE & 0x07FFFFFF)) >> 2;
    
    if (im_idx < RAM_SIZE) {
        uint32_t raw_instr = ram[im_idx];   
        IF_ID_instr = (ap_uint<32>)raw_instr;
    } else {
        IF_ID_instr = 0; 
    }
    IF_ID_pc = pc;

    if(CORE_DEBUG) {
        std::cout << "\n------------------------------------------------------------\n";
        std::cout << "[FETCH] PC=0x" << std::hex << (unsigned)pc 
                  << " Instr=0x" << (unsigned)IF_ID_instr << std::dec << "\n";
    }
    return IF_ID_instr;
}

// Returns the opcode decoded
ap_uint<7> decode() {
    #pragma HLS INLINE
    ap_uint<32> instr = IF_ID_instr; 
    ID_EX_opcode = instr.range(6, 0);
    ID_EX_rd     = instr.range(11, 7);
    ID_EX_funct3 = instr.range(14, 12);
    ID_EX_rs1    = instr.range(19, 15);
    ID_EX_rs2    = instr.range(24, 20);
    ID_EX_funct7 = instr.range(31, 25);
    ID_EX_instr = instr; 

    // Uniform Switch for Immediate Extraction
    switch (ID_EX_opcode) {
        case 0x23: ID_EX_imm = sextS(instr); break; // Store
        case 0x63: ID_EX_imm = sextB(instr); break; // Branch
        case 0x6F: ID_EX_imm = sextJ(instr); break; // JAL
        case 0x2F: ID_EX_imm = 0; break;            // Atomics (No immediate)
        default:   ID_EX_imm = sextI(instr); break; // All others (I-type, JALR, Load, ALU-I)
    }
    
    ID_EX_pc = IF_ID_pc;
    ID_EX_rs1_val = (ID_EX_rs1 == 0) ? (ap_int<32>)0 : regfile[ID_EX_rs1];
    ID_EX_rs2_val = (ID_EX_rs2 == 0) ? (ap_int<32>)0 : regfile[ID_EX_rs2];

    if (CORE_DEBUG) {
        std::cout << "[DECODE] Opcode=0x" << std::hex << (int)ID_EX_opcode 
                  << " Rd=" << (int)ID_EX_rd << std::dec << "\n";
    }
    return ID_EX_opcode;
}

// Returns the ALU result
ap_int<32> execute() {
    #pragma HLS INLINE
    ap_int<32> rs1_val = ID_EX_rs1_val;
    ap_int<32> rs2_val = ID_EX_rs2_val;

    EX_MEM_alu_result = 0;
    EX_MEM_rd         = ID_EX_rd;
    EX_MEM_mem_read   = false;
    EX_MEM_mem_write  = false;
    EX_MEM_reg_write  = false;
    EX_MEM_store_val  = rs2_val;
    EX_MEM_funct3     = ID_EX_funct3;
    EX_MEM_is_trap    = false; 

    EX_MEM_is_atomic  = false;
    EX_MEM_atomic_op  = 0;

    switch ((unsigned)ID_EX_opcode) {
    case 0x2F: { // A-Extension (Atomics)
        if (ENABLE_A_EXTENSION) {
            // funct3 must be 0x2 (32-bit word) for RV32A
            if (ID_EX_funct3 == 0x2) {
                EX_MEM_is_atomic = true;
                EX_MEM_atomic_op = ID_EX_funct7.range(6, 2); // Extract top 5 bits
                EX_MEM_alu_result = rs1_val; // Memory Address
                EX_MEM_store_val = rs2_val;  // Source value (for SC, SWAP, ADD, etc.)
                EX_MEM_reg_write = true;     // Most atomics write back to Rd
            } else {
                EX_MEM_is_trap = true; // Illegal width
            }
        } else {
            EX_MEM_is_trap = true; // Extension disabled
        }
        break;
    }
    case 0x33: { // R-type
            ap_uint<5> shamt = rs2_val.range(4,0);
            EX_MEM_reg_write = true;
            
            switch ((unsigned)ID_EX_funct7) {
                case 0x00:
                    switch ((unsigned)ID_EX_funct3) {
                        case 0x0: EX_MEM_alu_result = rs1_val + rs2_val; break; // ADD
                        case 0x1: EX_MEM_alu_result = rs1_val << shamt;  break; // SLL
                        case 0x2: EX_MEM_alu_result = (rs1_val < rs2_val) ? 1 : 0; break; // SLT
                        case 0x3: EX_MEM_alu_result = ((ap_uint<32>)rs1_val < (ap_uint<32>)rs2_val) ? 1 : 0; break; // SLTU
                        case 0x4: EX_MEM_alu_result = rs1_val ^ rs2_val; break; // XOR
                        case 0x5: EX_MEM_alu_result = (ap_uint<32>)rs1_val >> shamt; break; // SRL
                        case 0x6: EX_MEM_alu_result = rs1_val | rs2_val; break; // OR
                        case 0x7: EX_MEM_alu_result = rs1_val & rs2_val; break; // AND
                        default:  EX_MEM_reg_write = false; EX_MEM_is_trap = true; break; 
                    }
                    break;
                case 0x20:
                    switch ((unsigned)ID_EX_funct3) {
                        case 0x0: EX_MEM_alu_result = rs1_val - rs2_val; break; // SUB
                        case 0x5: EX_MEM_alu_result = rs1_val >> shamt; break; // SRA
                        default:  EX_MEM_reg_write = false; EX_MEM_is_trap = true; break;
                    }
                    break;
                case 0x01: // M-Extension (MUL, DIV, REM)
                    if (ENABLE_M_EXTENSION) {
                        // This logic is ONLY synthesized if flag is TRUE.
                        // ---------------------------------------------------------
                        // OPTIMIZATION: Manually share ONE divider for all ops.
                        // This prevents HLS from building 4 separate divider units.
                        // ---------------------------------------------------------
                        // 1. Identify Operation Properties
                        bool is_div  = (ID_EX_funct3 == 0x4); // DIV
                        bool is_divu = (ID_EX_funct3 == 0x5); // DIVU
                        bool is_rem  = (ID_EX_funct3 == 0x6); // REM
                        bool is_remu = (ID_EX_funct3 == 0x7); // REMU
                        
                        bool is_div_op = is_div || is_divu || is_rem || is_remu;
                        bool is_signed = is_div || is_rem;

                        // 2. Prepare Operands (Convert Signed to Unsigned Absolute)
                        bool sign_a = rs1_val[31];
                        bool sign_b = rs2_val[31];
                        
                        ap_uint<32> u_a = (is_signed && sign_a) ? (ap_uint<32>)(-rs1_val) : (ap_uint<32>)rs1_val;
                        ap_uint<32> u_b = (is_signed && sign_b) ? (ap_uint<32>)(-rs2_val) : (ap_uint<32>)rs2_val;

                        // 3. The Heavy Math (Instantiates ONLY ONE Divider)
                        ap_uint<32> u_quot = 0;
                        ap_uint<32> u_rem  = 0;

                        // Only run the divider if it's a division op (saves power/logic on MULs)
                        if (is_div_op) {
                            if (u_b != 0) {
                                u_quot = u_a / u_b;
                                // OPTIMIZATION: Calculate Remainder using Multiplier (uses DSPs)
                                // instead of a second Divider core (uses ~2000 FFs).
                                // Formula: Rem = A - (Quotient * B)
                                u_rem = u_a - (u_quot * u_b);
                            } else {
                                // RISC-V Spec for Div by Zero
                                u_quot = -1;  // All 1s
                                u_rem  = u_a; // Remainder is dividend
                            }
                        }

                        // 4. Fix Signs for Results
                        ap_int<32> res_div = 0;
                        ap_int<32> res_rem = 0;

                        if (is_signed) {
                            // Quotient is negative if signs differ
                            res_div = ((sign_a ^ sign_b) && u_b != 0) ? (ap_int<32>)-u_quot : (ap_int<32>)u_quot;
                            // Remainder sign follows the Dividend (rs1)
                            res_rem = (sign_a && u_b != 0) ? (ap_int<32>)-u_rem : (ap_int<32>)u_rem;
                            
                            // Handle Overflow Case (-MAX / -1)
                            if (u_b == 1 && sign_b && !sign_a && rs1_val == (ap_int<32>)0x80000000) {
                                res_div = rs1_val;
                                res_rem = 0;
                            }
                        } else {
                            res_div = (ap_int<32>)u_quot;
                            res_rem = (ap_int<32>)u_rem;
                        }

                        // 5. Select Output
                        switch ((unsigned)ID_EX_funct3) {
                            case 0x0: EX_MEM_alu_result = rs1_val * rs2_val; break; // MUL
                            case 0x1: EX_MEM_alu_result = (ap_int<32>)(((ap_int<64>)rs1_val * (ap_int<64>)rs2_val) >> 32); break; // MULH
                            case 0x2: EX_MEM_alu_result = (ap_int<32>)(((ap_int<64>)rs1_val * (ap_uint<64>)((ap_uint<32>)rs2_val)) >> 32); break; // MULHSU
                            case 0x3: EX_MEM_alu_result = (ap_int<32>)(((ap_uint<64>)((ap_uint<32>)rs1_val) * (ap_uint<64>)((ap_uint<32>)rs2_val)) >> 32); break; // MULHU
                            case 0x4: EX_MEM_alu_result = res_div; break; // DIV
                            case 0x5: EX_MEM_alu_result = res_div; break; // DIVU
                            case 0x6: EX_MEM_alu_result = res_rem; break; // REM
                            case 0x7: EX_MEM_alu_result = res_rem; break; // REMU
                        }
                    } else {
                        // If disabled, this path becomes an Illegal Instruction Trap.
                        EX_MEM_reg_write = false; 
                        EX_MEM_is_trap = true; 
                    }
                    break;
                default:
                    EX_MEM_reg_write = false; 
                    EX_MEM_is_trap = true; 
                    break;
            }
            break;
    }
    case 0x17: { // AUIPC
            ap_int<32> imm20 = (ap_int<32>)(ID_EX_instr & 0xFFFFF000);
            EX_MEM_alu_result = (ap_int<32>)ID_EX_pc + imm20;
            EX_MEM_reg_write  = (ID_EX_rd != 0);
            break;
    }
    case 0x13: { // I-type
            if (ID_EX_rd == 0 && ID_EX_rs1 == 0 && ID_EX_imm == 0) { // NOP
                EX_MEM_reg_write = false;
            } else {
                EX_MEM_reg_write = true; 
            }
            ap_uint<5> shamt = ID_EX_imm.range(4,0); 
            
            switch ((unsigned)ID_EX_funct3) {
                case 0x0: EX_MEM_alu_result = rs1_val + ID_EX_imm; break; // ADDI
                case 0x1: EX_MEM_alu_result = rs1_val << shamt;  break; // SLLI
                case 0x2: EX_MEM_alu_result = (rs1_val < ID_EX_imm) ? 1 : 0; break; // SLTI
                case 0x3: EX_MEM_alu_result = ((ap_uint<32>)rs1_val < (ap_uint<32>)ID_EX_imm) ? 1 : 0; break; // SLTIU
                case 0x4: EX_MEM_alu_result = rs1_val ^ ID_EX_imm; break; // XORI
                case 0x5: 
                    if (ID_EX_instr[30] == 0) EX_MEM_alu_result = (ap_uint<32>)rs1_val >> shamt; // SRLI
                    else EX_MEM_alu_result = rs1_val >> shamt; // SRAI
                    break;
                case 0x6: EX_MEM_alu_result = rs1_val | ID_EX_imm; break; // ORI
                case 0x7: EX_MEM_alu_result = rs1_val & ID_EX_imm; break; // ANDI
                default: EX_MEM_reg_write = false; EX_MEM_is_trap = true; break;
            }
            break;
    }
    case 0x03: { // Load
            EX_MEM_alu_result = rs1_val + ID_EX_imm;
            EX_MEM_mem_read   = true;
            EX_MEM_reg_write  = true;
            break;
    }
    case 0x23: { // Store
            EX_MEM_alu_result = rs1_val + ID_EX_imm;
            EX_MEM_mem_write  = true;
            break;
    }
    case 0x63: { // Branch
            ap_int<32> tgt = (ap_int<32>)ID_EX_pc + ID_EX_imm;
            bool branch_taken = false;
            
            switch ((unsigned)ID_EX_funct3) {
                case 0x0: branch_taken = (rs1_val == rs2_val); break; // BEQ
                case 0x1: branch_taken = (rs1_val != rs2_val); break; // BNE
                case 0x4: branch_taken = (rs1_val < rs2_val);  break; // BLT
                case 0x5: branch_taken = (rs1_val >= rs2_val); break; // BGE
                case 0x6: branch_taken = ((ap_uint<32>)rs1_val < (ap_uint<32>)rs2_val);  break; // BLTU
                case 0x7: branch_taken = ((ap_uint<32>)rs1_val >= (ap_uint<32>)rs2_val); break; // BGEU
                default:  branch_taken = false; break;
            }

            if(branch_taken) {
                next_pc = (ap_uint<32>)tgt;
                took_branch_or_jump = true;
            }
            break;
    }
    case 0x6F: { // JAL
            EX_MEM_alu_result = (ap_int<32>)(ID_EX_pc + 4);
            next_pc = (ap_uint<32>)((ap_int<32>)ID_EX_pc + ID_EX_imm);
            took_branch_or_jump = true;
            EX_MEM_reg_write    = (ID_EX_rd != 0);
            break;
    }
    case 0x67: { // JALR
            EX_MEM_alu_result = (ap_int<32>)(ID_EX_pc + 4); 
            next_pc = (ap_uint<32>)(((ap_int<32>)rs1_val + ID_EX_imm) & (~1));
            took_branch_or_jump = true;
            EX_MEM_reg_write    = (ID_EX_rd != 0);
            break;
    }
    case 0x37:{ // LUI
            EX_MEM_alu_result = (ap_int<32>)(ID_EX_instr & 0xFFFFF000);
            EX_MEM_reg_write  = (ID_EX_rd != 0);
            break;
    }
    case 0x73: { // System
        unsigned csr_addr = ID_EX_imm.range(11,0); 
        ap_uint<5> rs1_imm = ID_EX_rs1; 
        ap_uint<32> trap_cause = 0;
        
        ap_uint<32> csr_read_val = 0;
        
        // CSR Read Switch
        switch (csr_addr) {
            case 0x305: csr_read_val = csr_mtvec; break;
            case 0x340: csr_read_val = csr_mscratch; break; // mscratch
            case 0x341: csr_read_val = csr_mepc; break;
            case 0x342: csr_read_val = csr_mcause; break;
            case 0xB00: csr_read_val = csr_mcycle; break;   // mcycle
            case 0xC00: csr_read_val = csr_mcycle; break;   // cycle (user alias)
            case 0xB02: csr_read_val = csr_minstret; break; // minstret
            case 0xC02: csr_read_val = csr_minstret; break; // instret (user alias)
            case 0xF14: csr_read_val = 0; break;            // mhartid (Core 0)
            case 0x300: csr_read_val = csr_mstatus; break;  // mstatus
            case 0x304: csr_read_val = csr_mie; break;      // mie (IRQ Enable)
            case 0x344: csr_read_val = csr_mip; break;      // mip (IRQ Pending)
            default:    csr_read_val = 0; break;
        }
        
        EX_MEM_reg_write = false; 

        switch ((unsigned)ID_EX_funct3) {
            case 0x0: // ECALL / EBREAK
                if (ID_EX_imm == 0x000) { 
                    // Check for Exit System Call
                    if (regfile[17] == 93) {
                        is_finished = true;
                        std::cout << "[CORE DEBUG] Exit Condition Met! Stopping Simulation." << std::endl;
                    }
                    EX_MEM_is_trap = true; 
                    trap_cause = 11; 
                } // ECALL
                else if (ID_EX_imm == 0x001) { EX_MEM_is_trap = true; trap_cause = 3; } // EBREAK
                else if (ID_EX_imm == 0x105) { // WFI (Wait For Interrupt)
                    // 1. Check the Enable Bits
                    bool global_enable = (csr_mstatus >> 3) & 1;
                    bool timer_enable  = (csr_mie >> 7) & 1;

                    // 2. Apply the Nudge
                    if (csr_mcycle > 500000) { 
                        mtimecmp = (ap_uint<64>)csr_mcycle + 100;
                        
                        // 3. Print the diagnostic info
                        std::cout << "[SIM-HACK] WFI at Cycle " << std::hex << (uint64_t)csr_mcycle 
                                  << " | MIE (Global): " << (int)global_enable 
                                  << " | MTIE (Timer): " << (int)timer_enable 
                                  << std::dec << std::endl << std::flush;
                    }
                }
                else if (ID_EX_imm == 0x302) { // MRET
                    // 1. Restore PC from MEPC
                    next_pc = (ap_uint<32>)csr_mepc;
                    took_branch_or_jump = true;

                    // 2. Manage Interrupt Enable Bits in mstatus
                    bool mpie = (csr_mstatus >> 7) & 1; // Extract previous interrupt state
                    
                    if(mpie) csr_mstatus |= (1 << 3);   // Restore MIE to 1
                    else     csr_mstatus &= ~(1 << 3);  // Restore MIE to 0
                    
                    csr_mstatus |= (1 << 7);            // Set MPIE to 1 (standard requirement)

                    // 3. Prevent writing to destination register
                    EX_MEM_reg_write = false; 
                    
                    if(CORE_DEBUG) std::cout << "[MRET] Returning to 0x" << std::hex << (int)next_pc << std::dec << "\n";
                }
                break;
            case 0x1: // CSRRW
                EX_MEM_alu_result = csr_read_val; 
                EX_MEM_reg_write = (ID_EX_rd != 0);
                switch (csr_addr) {
                    case 0x305: csr_mtvec = rs1_val; break;
                    case 0x340: csr_mscratch = rs1_val; break; // mscratch
                    case 0x341: csr_mepc  = rs1_val; break;
                    case 0x342: csr_mcause = rs1_val; break;
                    case 0x300: csr_mstatus = rs1_val; break;
                    case 0x304: csr_mie = rs1_val; break;      // mie
                    case 0x344: csr_mip = rs1_val; break;      // mip
                }
                break;
            case 0x2: // CSRRS
                EX_MEM_alu_result = csr_read_val; 
                EX_MEM_reg_write = (ID_EX_rd != 0);
                if (ID_EX_rs1 != 0) { 
                    ap_uint<32> new_val = csr_read_val | rs1_val;

                    //if (csr_addr == 0x300 || csr_addr == 0x304) {
                    //     std::cout << "[CSR-WRITE] Addr: 0x" << std::hex << csr_addr 
                    //               << " | Old: 0x" << (uint32_t)csr_read_val 
                    //               << " | New: 0x" << (uint32_t)new_val 
                    //               << std::dec << std::endl << std::flush;
                    //}

                    switch (csr_addr) {
                        case 0x305: csr_mtvec = new_val; break;
                        case 0x340: csr_mscratch = new_val; break; // mscratch
                        case 0x341: csr_mepc  = new_val; break;
                        case 0x342: csr_mcause = new_val; break;
                        case 0x300: csr_mstatus = new_val; break;
                        case 0x304: csr_mie = new_val; break;      // mie
                        case 0x344: csr_mip = new_val; break;      // mip
                    }
                }
                break;
            case 0x3: // CSRRC (Atomic Read and Clear Bits)
                EX_MEM_alu_result = csr_read_val;
                EX_MEM_reg_write = (ID_EX_rd != 0);
                if (ID_EX_rs1 != 0) {
                    ap_uint<32> new_val = csr_read_val & ~rs1_val;
                    switch (csr_addr) {
                        case 0x305: csr_mtvec = new_val; break;
                        case 0x340: csr_mscratch = new_val; break; // mscratch
                        case 0x341: csr_mepc = new_val; break;
                        case 0x342: csr_mcause = new_val; break;
                        case 0x300: csr_mstatus = new_val; break;
                        case 0x304: csr_mie = new_val; break;      // mie
                        case 0x344: csr_mip = new_val; break;      // mip
                    }
                }
                break;
            case 0x5: // CSRRWI
                EX_MEM_alu_result = csr_read_val; 
                EX_MEM_reg_write = (ID_EX_rd != 0);
                switch (csr_addr) {
                    case 0x305: csr_mtvec = (ap_uint<32>)rs1_imm; break;
                    case 0x340: csr_mscratch = (ap_uint<32>)rs1_imm; break; // mscratch
                    case 0x341: csr_mepc  = (ap_uint<32>)rs1_imm; break;
                    case 0x342: csr_mcause = (ap_uint<32>)rs1_imm; break;
                    case 0x300: csr_mstatus = (ap_uint<32>)rs1_imm; break;
                    case 0x304: csr_mie = (ap_uint<32>)rs1_imm; break;      // mie
                    case 0x344: csr_mip = (ap_uint<32>)rs1_imm; break;      // mip
                }
                break;
            case 0x6: // CSRRSI
                EX_MEM_alu_result = csr_read_val;
                EX_MEM_reg_write = (ID_EX_rd != 0);
                if (rs1_imm != 0) { 
                    ap_uint<32> new_val = csr_read_val | rs1_imm; 
                    switch (csr_addr) {
                        case 0x305: csr_mtvec = new_val; break;
                        case 0x340: csr_mscratch = new_val; break; // mscratch
                        case 0x341: csr_mepc  = new_val; break;
                        case 0x342: csr_mcause = new_val; break;
                        case 0x300: csr_mstatus = new_val; break;
                        case 0x304: csr_mie = new_val; break;      // mie
                        case 0x344: csr_mip = new_val; break;      // mip
                    }
                }
                break;
            case 0x7: // CSRRCI
                EX_MEM_alu_result = csr_read_val;
                EX_MEM_reg_write = (ID_EX_rd != 0);
                if (rs1_imm != 0) { 
                    ap_uint<32> new_val = csr_read_val & ~rs1_imm; 
                    switch (csr_addr) {
                        case 0x305: csr_mtvec = new_val; break;
                        case 0x340: csr_mscratch = new_val; break; // mscratch
                        case 0x341: csr_mepc  = new_val; break;
                        case 0x342: csr_mcause = new_val; break;
                        case 0x300: csr_mstatus = new_val; break;
                        case 0x304: csr_mie = new_val; break;      // mie
                        case 0x344: csr_mip = new_val; break;      // mip
                    }
                }
                break;
            default:
                EX_MEM_is_trap = true;
                trap_cause = 2; 
                break;
        }

        if (EX_MEM_is_trap) {
            csr_mepc = ID_EX_pc;       
            csr_mcause = trap_cause;  
            next_pc = csr_mtvec;      
            took_branch_or_jump = true; 
            EX_MEM_reg_write = false; 
        }
        break;
    } 
    case 0x0F: { // FENCE and FENCE.I
        EX_MEM_reg_write = false; 

        switch ((unsigned)ID_EX_funct3) {
            case 0x1: // FENCE.I
                if(CORE_DEBUG) std::cout << "[FENCE.I] Synchronizing Instruction Stream\n";
                break;
            default: // FENCE
                if(CORE_DEBUG) std::cout << "[FENCE] Memory Barrier\n";
                break;
        }
        break;
    }

    default:
            EX_MEM_is_trap = true;
            csr_mepc = ID_EX_pc;       
            csr_mcause = 2; 
            next_pc = csr_mtvec;     
            took_branch_or_jump = true; 
            EX_MEM_reg_write = false; 
            break;
    }

    if (CORE_DEBUG) {
        std::cout << "[EXEC] ALU=0x" << std::hex << (int)EX_MEM_alu_result << std::dec << "\n";
    }
    return EX_MEM_alu_result;
}

// Returns the value that would be written to register (or loaded val)
ap_int<32> memory(volatile uint32_t* ram) {
    #pragma HLS INLINE
    MEM_WB_is_trap = EX_MEM_is_trap; 
    if (EX_MEM_is_trap) {
        EX_MEM_mem_read = false;
        EX_MEM_mem_write = false;
        EX_MEM_reg_write = false; 
    }

    MEM_WB_value     = EX_MEM_alu_result;  
    MEM_WB_rd        = EX_MEM_rd;
    MEM_WB_reg_write = EX_MEM_reg_write;

    unsigned ea_u  = (unsigned)EX_MEM_alu_result; 
    unsigned phys_ea = ea_u & 0x07FFFFFF; 
    unsigned d_idx = (phys_ea - (DRAM_BASE & 0x07FFFFFF)) >> 2;
    unsigned byte_off = phys_ea & 0x3;

    // =============================================================
    // ATOMIC MEMORY OPERATIONS (A-EXTENSION)
    // =============================================================
    if (EX_MEM_is_atomic) {
        if (d_idx >= RAM_SIZE) {
            MEM_WB_value = 0; // Fault
        } else {
            uint32_t loaded_raw = ram[d_idx]; // Read Current Memory
            ap_int<32> loaded_val = (ap_int<32>)loaded_raw;
            ap_int<32> write_val = 0;
            bool do_write = false;

            // Load Reserved (LR.W)
            if (EX_MEM_atomic_op == 0x02) { 
                lr_addr = ea_u;
                lr_valid = true;
                MEM_WB_value = loaded_val; // Result is the loaded value
                do_write = false; // LR does not write to RAM
                if(CORE_DEBUG) std::cout << "[AMO] LR at 0x" << std::hex << ea_u << std::dec << "\n";
            } 
            // Store Conditional (SC.W)
            else if (EX_MEM_atomic_op == 0x03) {
                if (lr_valid && lr_addr == ea_u) {
                    write_val = EX_MEM_store_val;
                    do_write = true;
                    MEM_WB_value = 0; // Success (0)
                    lr_valid = false; // Invalidate
                } else {
                    do_write = false;
                    MEM_WB_value = 1; // Failure (1)
                }
                if(CORE_DEBUG) std::cout << "[AMO] SC at 0x" << std::hex << ea_u << (do_write ? " Success" : " Fail") << std::dec << "\n";
            }
            // AMO Read-Modify-Write Operations
            else {
                ap_int<32> op_b = EX_MEM_store_val;
                do_write = true;
                switch (EX_MEM_atomic_op) {
                    case 0x01: write_val = op_b; break; // AMOSWAP
                    case 0x00: write_val = loaded_val + op_b; break; // AMOADD
                    case 0x04: write_val = loaded_val ^ op_b; break; // AMOXOR
                    case 0x0C: write_val = loaded_val & op_b; break; // AMOAND
                    case 0x08: write_val = loaded_val | op_b; break; // AMOOR
                    case 0x10: write_val = (loaded_val < op_b) ? loaded_val : op_b; break; // AMOMIN
                    case 0x14: write_val = (loaded_val > op_b) ? loaded_val : op_b; break; // AMOMAX
                    case 0x18: write_val = ((ap_uint<32>)loaded_val < (ap_uint<32>)op_b) ? loaded_val : op_b; break; // AMOMINU
                    case 0x1C: write_val = ((ap_uint<32>)loaded_val > (ap_uint<32>)op_b) ? loaded_val : op_b; break; // AMOMAXU
                    default: do_write = false; break;
                }
                MEM_WB_value = loaded_val; // AMOs write original value to Rd
            }

            if (do_write) {
                ram[d_idx] = (uint32_t)write_val;
                // Invalidate reservation on any write
                lr_valid = false; 
            }
        }
    }
    // =============================================================
    // NORMAL LOAD
    // =============================================================
    else if (EX_MEM_mem_read) {
        // ----------------------------------------------------------------
        // MMIO READ: UART Sniffer
        // ----------------------------------------------------------------
        if ((ea_u & 0xFFFFF000) == 0x10000000) {
            // Default to "Ready" (0x60) for ANY status register read
            // This tricks the kernel regardless of where it looks (0x05, 0x14, etc.)
            MEM_WB_value = 0x60; 
            MEM_WB_reg_write = true;
            
            #ifdef __SYNTHESIS__
            #else
                //std::cout << "[UART-DEBUG] Read from 0x" << std::hex << ea_u << std::dec << "\n";
            #endif
            return MEM_WB_value;
        }
        // mtimecmp (0x2004000) - Timer Compare Register
        if (phys_ea == 0x2004000) {
            MEM_WB_value = (ap_int<32>)(mtimecmp & 0xFFFFFFFF); // Low 32 bits
            MEM_WB_reg_write = true;
            return MEM_WB_value;
        }
        if (phys_ea == 0x2004004) {
            MEM_WB_value = (ap_int<32>)(mtimecmp >> 32); // High 32 bits
            MEM_WB_reg_write = true;
            return MEM_WB_value;
        }
        
        // mtime (0x200BFF8) - Current Time (Aliased to csr_mcycle)
        // Linux reads this to know "what time is it?"
        if (phys_ea == 0x200BFF8) {
            MEM_WB_value = (ap_int<32>)(csr_mcycle & 0xFFFFFFFF); // Low 32 bits
            MEM_WB_reg_write = true;
            return MEM_WB_value;
        }
        if (phys_ea == 0x200BFFC) {
            // Since csr_mcycle is 32-bit in this core, high bits are 0.
            // Ideally, csr_mcycle should be 64-bit for long uptimes.
            MEM_WB_value = 0; 
            MEM_WB_reg_write = true;
            return MEM_WB_value;
        }

        // ----------------------------------------------------------------
        // STANDARD RAM READ
        // ----------------------------------------------------------------
        if (d_idx >= RAM_SIZE) {
            if(CORE_DEBUG) std::cerr << "[MEM] Load OOB: EA=0x" << std::hex << ea_u << "\n";
            MEM_WB_value = 0;
        } else {
            uint32_t raw_w0 = ram[d_idx];
            uint32_t raw_w1 = (d_idx + 1 < RAM_SIZE) ? (uint32_t)ram[d_idx + 1] : 0;

            ap_uint<32> word0 = (ap_uint<32>)raw_w0;
            ap_uint<32> word1 = (ap_uint<32>)raw_w1;
            
            // Misalignment Logic
            ap_uint<32> raw_val_32 = (word0 >> (byte_off * 8)) | (word1 << ((4 - byte_off) * 8));
            ap_int<32> loaded_val = 0;
            MEM_WB_reg_write = true;

            switch ((unsigned)EX_MEM_funct3) { 
                case 0: loaded_val = (ap_int<32>)((ap_int<8>)raw_val_32.range(7, 0)); break; // LB
                case 1: loaded_val = (ap_int<32>)((ap_int<16>)raw_val_32.range(15, 0)); break; // LH
                case 2: loaded_val = (ap_int<32>)raw_val_32; break; // LW
                case 4: loaded_val = (ap_int<32>)((ap_uint<32>)raw_val_32.range(7, 0)); break; // LBU
                case 5: loaded_val = (ap_int<32>)((ap_uint<32>)raw_val_32.range(15, 0)); break; // LHU
                default: MEM_WB_reg_write = false; break; 
            }
            MEM_WB_value = loaded_val; 
        }
    }
    // =============================================================
    // NORMAL STORE
    // =============================================================
    else if (EX_MEM_mem_write) {
        // Any standard write invalidates a Load Reservation
        lr_valid = false;
        // ----------------------------------------------------------------
        // MMIO: UART Write Sniffer (Debugs ALL 0x44A... writes)
        // ----------------------------------------------------------------
        if ((ea_u & 0xFFFFF000) == 0x10000000) { 
            #ifdef __SYNTHESIS__
            #else
                // Print the details of ANY write to this region
                //std::cout << "[UART-DEBUG] Write to 0x" << std::hex << ea_u 
                //        << " Val=0x" << EX_MEM_store_val << std::dec << "\n";

                // If it's a write to the Data Register (Base or Base+4?), print char
                // We verify both 0x00 (Byte mode) and 0x00 (Word mode)
                if ((ea_u & 0xFF) == 0x00) {
                    char c = (char)(EX_MEM_store_val & 0xFF);
                    std::cout << c << std::flush; 
                }
            #endif
            MEM_WB_reg_write = false; 
            return 0;
        }

        // ----------------------------------------------------------------
        // MMIO: CLINT (Timer Compare)
        // ----------------------------------------------------------------
        // Lower 32-bits of mtimecmp
        if (phys_ea == 0x2004000) {
            mtimecmp = (mtimecmp & 0xFFFFFFFF00000000) | (ap_uint<32>)EX_MEM_store_val;
            if(CORE_DEBUG) std::cout << "[CLINT] mtimecmp Low Update: " << std::hex << mtimecmp << std::dec << "\n";
            return MEM_WB_value;
        }
        // Upper 32-bits of mtimecmp
        if (phys_ea == 0x2004004) {
             mtimecmp = (mtimecmp & 0x00000000FFFFFFFF) | ((ap_uint<64>)EX_MEM_store_val << 32);
             if(CORE_DEBUG) std::cout << "[CLINT] mtimecmp High Update: " << std::hex << mtimecmp << std::dec << "\n";
             return MEM_WB_value;
        }

        if (d_idx >= RAM_SIZE) {
            if(CORE_DEBUG) std::cerr << "[MEM] Store OOB: EA=0x" << std::hex << ea_u << "\n";
        } else {
            // Read-Modify-Write
            uint32_t raw_w0 = ram[d_idx];
            ap_uint<32> word0 = (ap_uint<32>)raw_w0;
            
            ap_uint<32> store_val = (ap_uint<32>)EX_MEM_store_val;
            ap_uint<32> mask0 = 0;
            ap_uint<32> mask1 = 0;
            
            switch ((unsigned)EX_MEM_funct3) {
                case 0: // SB
                    mask0 = 0xFF << (byte_off * 8);
                    break;
                case 1: { // SH
                    ap_uint<64> full_mask = (ap_uint<64>)0xFFFF << (byte_off * 8);
                    mask0 = full_mask.range(31, 0);
                    mask1 = full_mask.range(63, 32);
                    break;
                }
                case 2: { // SW
                    ap_uint<64> full_mask = (ap_uint<64>)0xFFFFFFFF << (byte_off * 8);
                    mask0 = full_mask.range(31, 0);
                    mask1 = full_mask.range(63, 32);
                    break;
                }
            }

            // Modify Word 0
            word0 = (word0 & ~mask0) | ((store_val << (byte_off * 8)) & mask0);
            ram[d_idx] = (uint32_t)word0; // Write back as standard uint32_t

            // Modify Word 1 (Boundary Crossing)
            if (mask1 != 0 && d_idx + 1 < RAM_SIZE) {
                uint32_t raw_w1 = ram[d_idx + 1];
                ap_uint<32> word1 = (ap_uint<32>)raw_w1;
                
                word1 = (word1 & ~mask1) | ((store_val >> ((4-byte_off)*8)) & mask1);
                ram[d_idx + 1] = (uint32_t)word1; 
            }
            
            // ----------------------------------------------------------------
            // HTIF INTERCEPTOR (Prevents Syscall Deadlock)
            // ----------------------------------------------------------------
            if (phys_ea == 0x1000) {
                unsigned fromhost_idx = d_idx + 16; 
                if (fromhost_idx < RAM_SIZE) {
                    ram[fromhost_idx] = 1; 
                }
            }
            // ----------------------------------------------------------------
            
            if(CORE_DEBUG) std::cout << "[MEM] Stored 0x" << std::hex << (int)EX_MEM_store_val << " to 0x" << ea_u << std::dec << "\n";
        }
    }
    return MEM_WB_value;
}

// Returns the value written to register (for debug/pipeline tracking)
ap_int<32> writeback() {
    #pragma HLS INLINE
    ap_int<32> wb_val = 0;
    if (MEM_WB_reg_write && MEM_WB_rd != 0 && !MEM_WB_is_trap) {
        regfile[MEM_WB_rd] = MEM_WB_value;
        wb_val = MEM_WB_value;
        if(CORE_DEBUG) std::cout << "[WB] x" << (int)MEM_WB_rd << " <= 0x" << std::hex << (int)MEM_WB_value << std::dec << "\n";
    }
    regfile[0] = 0; 
    return wb_val;
}

// ------------------------------------------------------------
// Modified Step function within a loop (Unified Memory)
// ------------------------------------------------------------
void riscv_step(volatile uint32_t* ram, int* cycles_output) {
    // AXI Master Interface for RAM
    #pragma HLS INTERFACE m_axi port=ram offset=off depth=262144 bundle=gmem
    // Cycle Counter
    #pragma HLS INTERFACE s_axilite port=cycles_output bundle=control
    // AXI Lite Interface for Control
    #pragma HLS INTERFACE s_axilite port=return bundle=control
    #pragma HLS BIND_STORAGE variable=regfile type=ram_2p impl=lutram

    // Reset the finished flag at start of simulation
    is_finished = false;

    INSTRUCTION_LOOP: while(true) {
        #pragma HLS LOOP_TRIPCOUNT min=50 max=500000

        // ------------------ Steps for CSR ------------------
        csr_mcycle++;   // Always increment cycles
        csr_minstret++;

        // --- HEARTBEAT ---
        // Inside your loop
        if (csr_mcycle % 1000000 == 0) {
            #ifdef __SYNTHESIS__
            // Do nothing
            #else
            std::cout << "Cycle: " << std::dec << csr_mcycle 
                        << " | PC: 0x" << std::hex << pc 
                        << " | Instr: 0x" << IF_ID_instr << std::endl;
            #endif
        }
        // ------------------------------------

        // ------------------ INTERRUPT LOGIC  ------------------
        // 1. Check Timer Match (Casting mcycle to 64-bit for comparison)
        bool timer_irq = ((ap_uint<64>)csr_mcycle >= mtimecmp);

        // 2. Check Enables
        bool global_ie = (csr_mstatus >> 3) & 1; // MIE bit (3)
        bool timer_ie  = (csr_mie >> 7) & 1;     // MTIE bit (7)

        // 3. Update Pending Register (MIP)
        if (timer_irq) csr_mip |= (1 << 7); // Set MTIP (bit 7)
        else           csr_mip &= ~(1 << 7);

        // 4. Take Trap if conditions met
        if (timer_irq && global_ie && timer_ie) {
            if (CORE_DEBUG) std::cout << "[INT] Timer Interrupt! Jumping to Handler.\n";
            
            csr_mcause = 0x80000007; // Machine Timer Interrupt (Async + 7)
            csr_mepc   = pc;         // Save current PC
            
            // Disable Global Interrupts
            // Save MIE to MPIE (bit 7)
            bool old_mie = (csr_mstatus >> 3) & 1;
            if (old_mie) csr_mstatus |= (1 << 7);
            else         csr_mstatus &= ~(1 << 7);
            
            csr_mstatus &= ~(1 << 3); // Clear MIE
            
            // Jump to Handler (Using mtvec base address)
            pc = csr_mtvec; 
            
            // SKIP FETCH - Execute Handler immediately
            continue; 
        }
        // -----------------------------------------------------------------------

        // ------------------ Call Stages ------------------
        // We capture returns to prevent HLS optimization from pruning "unused" logic,
        ap_uint<32> f_out = fetch(ram);
        ap_uint<7>  d_out = decode();
        ap_int<32>  e_out = execute();
        ap_int<32>  m_out = memory(ram);
        ap_int<32>  w_out = writeback();

        // Create a dummy dependency chain to force HLS to keep these paths active
        // The volatile keyword prevents this calculation from being optimized away completely.
        volatile int dummy_accumulator = (int)f_out ^ (int)d_out ^ (int)e_out ^ (int)m_out ^ (int)w_out;

        // ========== NEXT PC ==========
        if (took_branch_or_jump) {
            pc = next_pc;
            took_branch_or_jump = false;  
        } else {
            pc += 4;
        }

        // Break loop if ecall exit detected
        if (is_finished) {
            *cycles_output = csr_mcycle;
            return;
        }
    }
}