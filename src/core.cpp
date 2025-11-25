#include <ap_int.h>
#include <iostream>
#include <cstring>
#include "core.h" 

// ------------------------------------------------------------
// Global Debug Switch
// ------------------------------------------------------------
bool CORE_DEBUG = false;

// ------------------------------------------------------------
// Global Architectural State
// ------------------------------------------------------------
ap_uint<32> pc = 0;
ap_int<32>  regfile[32];

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

// --- Global Variable Master Definitions ---
ap_uint<32> ENTRY_PC;

// ------------------------------------------------------------
// Initialization Function
// ------------------------------------------------------------
void riscv_init() {
    pc = ENTRY_PC;
    for(int i=0; i<32; i++) regfile[i] = 0;

    regfile[1] = (ap_int<32>)0xDEADBEEF;       
    regfile[0] = 0;                           
    regfile[2] = (ap_int<32>) (unsigned)DMEM_STACK_TOP;
    
    if(CORE_DEBUG) {
        std::cout << "[INIT] Core Reset. PC=0x" << std::hex << (unsigned)pc 
                  << ", SP=0x" << (unsigned)regfile[2] << std::dec << std::endl;
    }

    IF_ID_instr = 0;
    took_branch_or_jump = false;
    EX_MEM_is_trap = false;
    MEM_WB_is_trap = false;
}

// ------------------------------------------------------------
// Single Step Function (Unified Memory)
// ------------------------------------------------------------
void riscv_step(volatile uint32_t* ram) {
    // AXI Master Interface for RAM
    #pragma HLS INTERFACE m_axi port=ram offset=off depth=262144 bundle=gmem
    // AXI Lite Interface for Control
    #pragma HLS INTERFACE s_axilite port=return

    // ------------------ Immediate extractors ------------------
    auto sextI = [](ap_uint<32> insn) -> ap_int<32> {
        ap_int<12> imm12 = (ap_int<12>)(insn >> 20);
        return (ap_int<32>)imm12;
    };
    auto sextS = [](ap_uint<32> insn) -> ap_int<32> {
        ap_uint<7> hi = insn.range(31,25);
        ap_uint<5> lo = insn.range(11,7);
        ap_uint<12> u = ((ap_uint<12>)hi << 5) | (ap_uint<12>)lo;
        ap_int<12> s = (ap_int<12>)u;
        return (ap_int<32>)s;
    };
    auto sextB = [](ap_uint<32> insn) -> ap_int<32> {
        ap_uint<13> imm;
        imm[12]       = insn[31];       
        imm[11]       = insn[7];        
        imm.range(10,5) = insn.range(30,25); 
        imm.range(4,1)  = insn.range(11,8);  
        imm[0]          = 0;            
        return (ap_int<32>)((ap_int<13>)imm);
    };
    auto sextJ = [](ap_uint<32> insn) -> ap_int<32> {
        ap_uint<21> u = ((ap_uint<21>)insn[31]        << 20) |
                        ((ap_uint<21>)insn.range(30,21) << 1 ) |
                        ((ap_uint<21>)insn[20]        << 11) |
                        ((ap_uint<21>)insn.range(19,12) << 12);
        ap_int<21> s = (ap_int<21>)u;
        return (ap_int<32>)s;
    };

    // ========== FETCH ==========
    uint32_t phys_pc = pc & 0x000FFFFF; 
    uint32_t im_idx = (phys_pc - (DRAM_BASE & 0x000FFFFF)) >> 2;
    
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

    // ========== DECODE ==========
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
        default:   ID_EX_imm = sextI(instr); break; // All others (I-type, JALR, Load, ALU-I)
    }
    
    ID_EX_pc = IF_ID_pc;
    ID_EX_rs1_val = (ID_EX_rs1 == 0) ? (ap_int<32>)0 : regfile[ID_EX_rs1];
    ID_EX_rs2_val = (ID_EX_rs2 == 0) ? (ap_int<32>)0 : regfile[ID_EX_rs2];

    if (CORE_DEBUG) {
        std::cout << "[DECODE] Opcode=0x" << std::hex << (int)ID_EX_opcode 
                  << " Rd=" << (int)ID_EX_rd << std::dec << "\n";
    }

    // ========== EXECUTE ==========
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

    switch ((unsigned)ID_EX_opcode) {
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
            case 0x341: csr_read_val = csr_mepc; break;
            case 0x342: csr_read_val = csr_mcause; break;
            default:    csr_read_val = 0; break;
        }
        
        EX_MEM_reg_write = false; 

        switch ((unsigned)ID_EX_funct3) {
            case 0x0: // ECALL / EBREAK
                if (ID_EX_imm == 0x000) { EX_MEM_is_trap = true; trap_cause = 11; }
                break;
            case 0x1: // CSRRW
                EX_MEM_alu_result = csr_read_val; 
                EX_MEM_reg_write = (ID_EX_rd != 0);
                switch (csr_addr) {
                    case 0x305: csr_mtvec = rs1_val; break;
                    case 0x341: csr_mepc  = rs1_val; break;
                    case 0x342: csr_mcause = rs1_val; break;
                }
                break;
            case 0x2: // CSRRS
                EX_MEM_alu_result = csr_read_val; 
                EX_MEM_reg_write = (ID_EX_rd != 0);
                if (ID_EX_rs1 != 0) { 
                    ap_uint<32> new_val = csr_read_val | rs1_val; 
                    switch (csr_addr) {
                        case 0x305: csr_mtvec = new_val; break;
                        case 0x341: csr_mepc  = new_val; break;
                        case 0x342: csr_mcause = new_val; break;
                    }
                }
                break;
            case 0x5: // CSRRWI
                EX_MEM_alu_result = csr_read_val; 
                EX_MEM_reg_write = (ID_EX_rd != 0);
                switch (csr_addr) {
                    case 0x305: csr_mtvec = (ap_uint<32>)rs1_imm; break;
                    case 0x341: csr_mepc  = (ap_uint<32>)rs1_imm; break;
                    case 0x342: csr_mcause = (ap_uint<32>)rs1_imm; break;
                }
                break;
            case 0x6: // CSRRSI
                EX_MEM_alu_result = csr_read_val;
                EX_MEM_reg_write = (ID_EX_rd != 0);
                if (rs1_imm != 0) { 
                    ap_uint<32> new_val = csr_read_val | rs1_imm; 
                    switch (csr_addr) {
                        case 0x305: csr_mtvec = new_val; break;
                        case 0x341: csr_mepc  = new_val; break;
                        case 0x342: csr_mcause = new_val; break;
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
                        case 0x341: csr_mepc  = new_val; break;
                        case 0x342: csr_mcause = new_val; break;
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

    // ========== MEMORY (UNIFIED & MISALIGNED SUPPORT) ==========
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
    unsigned phys_ea = ea_u & 0x000FFFFF; 
    unsigned d_idx = (phys_ea - (DRAM_BASE & 0x000FFFFF)) >> 2;
    unsigned byte_off = phys_ea & 0x3;

    if (EX_MEM_mem_read) {
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
    else if (EX_MEM_mem_write) {
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
            
            if(CORE_DEBUG) std::cout << "[MEM] Stored 0x" << std::hex << (int)EX_MEM_store_val << " to 0x" << ea_u << std::dec << "\n";
        }
    }

    // ========== WRITEBACK ==========
    if (MEM_WB_reg_write && MEM_WB_rd != 0 && !MEM_WB_is_trap) {
        regfile[MEM_WB_rd] = MEM_WB_value;
        if(CORE_DEBUG) std::cout << "[WB] x" << (int)MEM_WB_rd << " <= 0x" << std::hex << (int)MEM_WB_value << std::dec << "\n";
    }
    regfile[0] = 0; 

    // ========== NEXT PC ==========
    if (took_branch_or_jump) {
        pc = next_pc;
        took_branch_or_jump = false;  
    } else {
        pc += 4;
    }
}