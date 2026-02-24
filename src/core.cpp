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
// Inter-Stage Data Structs
// ------------------------------------------------------------
struct FetchOut {
    ap_uint<32> instr;
    ap_uint<32> pc;
};

struct DecodeOut {
    ap_uint<7>  opcode;
    ap_uint<5>  rd;
    ap_uint<3>  funct3;
    ap_uint<5>  rs1;
    ap_uint<5>  rs2;
    ap_uint<7>  funct7;
    ap_int<32>  imm;
    ap_int<32>  pc;
    ap_int<32>  rs1_val;
    ap_int<32>  rs2_val;
    ap_uint<32> instr;
};

struct ExecOut {
    ap_int<32>  alu_result;
    ap_uint<5>  rd;
    bool        mem_read;
    bool        mem_write;
    bool        reg_write;
    ap_uint<3>  funct3;
    ap_int<32>  store_val;
    bool        is_trap;
    bool        is_atomic;
    ap_uint<5>  atomic_op;
    // Control signals
    bool        branch_taken;
    ap_uint<32> next_pc;
    bool        finished;
};

struct MemOut {
    ap_int<32>  value;
    ap_uint<5>  rd;
    bool        reg_write;
    bool        is_trap;
};

// ------------------------------------------------------------
// Address Translation Helper
// ------------------------------------------------------------
// Synthesis: m_axi base=0, index = full_byte_addr / 4
//   Allows core to reach DDR (0x80000000) AND UART (0x10000000)
// Simulation: ram[] is flat 128MB array, index = offset within DDR
static unsigned addr_to_idx(unsigned byte_addr) {
    #pragma HLS INLINE
    #ifdef __SYNTHESIS__
        return byte_addr >> 2;
    #else
        return ((byte_addr & 0x07FFFFFF) - (DRAM_BASE & 0x07FFFFFF)) >> 2;
    #endif
}

// ------------------------------------------------------------
// Global Architectural State
// ------------------------------------------------------------
#ifdef __SYNTHESIS__
    ap_uint<32> pc = 0x80000000; 
#else
    ap_uint<32> pc = 0;          
#endif
ap_int<32>  regfile[32];
bool is_finished = false;

// --- Load Reserved / Store Conditional State ---
ap_uint<32> lr_addr = 0;
bool lr_valid = false;

// --- CSRs ---
ap_uint<32> csr_mtvec = 0;
ap_uint<32> csr_mepc = 0;
ap_uint<32> csr_mcause = 0;
ap_uint<32> csr_mscratch = 0;

ap_uint<64> csr_mcycle = 0;   // Cycle Counter (0xB00/0xB80)
ap_uint<64> csr_minstret = 0; // Instructions Retired (0xB02/0xB82)
ap_uint<32> csr_mstatus = 0;  // Status Register (0x300)

// --- Interrupts & Timer ---
ap_uint<32> csr_mie = 0; // Interrupt Enable Register (0x304)
ap_uint<32> csr_mip = 0; // Interrupt Pending Register (0x344)
ap_uint<64> mtimecmp = 0xFFFFFFFFFFFFFFFF;

// --- Sink CSRs (accept writes, minimal/no effect in M-mode-only core) ---
ap_uint<32> csr_mtval = 0;         // Trap Value (0x343)
ap_uint<32> csr_medeleg = 0;       // Exception Delegation (0x302) - no S-mode, sink
ap_uint<32> csr_mideleg = 0;       // Interrupt Delegation (0x303) - no S-mode, sink
ap_uint<32> csr_mcountinhibit = 0; // Counter Inhibit (0x320)
ap_uint<32> csr_satp = 0;          // S-mode Address Translation (0x180) - sink

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

    is_finished = false;

    csr_mcycle = 0;
    csr_minstret = 0;
    csr_mstatus = 0;
    
    // Reset Timer state
    csr_mie = 0;
    csr_mip = 0;
    mtimecmp = 0xFFFFFFFFFFFFFFFF;

    // Reset Sink CSRs
    csr_mtval = 0;
    csr_medeleg = 0;
    csr_mideleg = 0;
    csr_mcountinhibit = 0;
    csr_satp = 0;

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
// CSR Read/Write Helpers
// ------------------------------------------------------------
ap_uint<32> csr_read(unsigned addr) {
    #pragma HLS INLINE
    switch (addr) {
        // Machine Information
        case 0xF11: return 0;                                          // mvendorid
        case 0xF12: return 0;                                          // marchid
        case 0xF13: return 0;                                          // mimpid
        case 0xF14: return 0;                                          // mhartid
        // Machine ISA
        case 0x301: return 0x40001101;                                 // misa: RV32IMA
        // Machine Trap Setup
        case 0x300: return csr_mstatus;                                // mstatus
        case 0x302: return csr_medeleg;                                // medeleg
        case 0x303: return csr_mideleg;                                // mideleg
        case 0x304: return csr_mie;                                    // mie
        case 0x305: return csr_mtvec;                                  // mtvec
        case 0x320: return csr_mcountinhibit;                          // mcountinhibit
        // Machine Trap Handling
        case 0x340: return csr_mscratch;                               // mscratch
        case 0x341: return csr_mepc;                                   // mepc
        case 0x342: return csr_mcause;                                 // mcause
        case 0x343: return csr_mtval;                                  // mtval
        case 0x344: return csr_mip;                                    // mip
        // Machine Counters
        case 0xB00: return (ap_uint<32>)csr_mcycle;                    // mcycle (low)
        case 0xB80: return (ap_uint<32>)(csr_mcycle >> 32);            // mcycleh (high)
        case 0xB02: return (ap_uint<32>)csr_minstret;                  // minstret (low)
        case 0xB82: return (ap_uint<32>)(csr_minstret >> 32);          // minstreth (high)
        // User Counter Aliases (read-only mirrors)
        case 0xC00: return (ap_uint<32>)csr_mcycle;                    // cycle
        case 0xC80: return (ap_uint<32>)(csr_mcycle >> 32);            // cycleh
        case 0xC02: return (ap_uint<32>)csr_minstret;                  // instret
        case 0xC82: return (ap_uint<32>)(csr_minstret >> 32);          // instreth
        // S-mode (sinks)
        case 0x180: return csr_satp;                                   // satp
        default:    return 0;
    }
}

void csr_write(unsigned addr, ap_uint<32> val) {
    #pragma HLS INLINE
    switch (addr) {
        // Machine Trap Setup
        case 0x300: csr_mstatus = val; break;        // mstatus
        case 0x302: csr_medeleg = val; break;         // medeleg (sink)
        case 0x303: csr_mideleg = val; break;         // mideleg (sink)
        case 0x304: csr_mie = val; break;             // mie
        case 0x305: csr_mtvec = val; break;           // mtvec
        case 0x320: csr_mcountinhibit = val; break;   // mcountinhibit (sink)
        // Machine Trap Handling
        case 0x340: csr_mscratch = val; break;        // mscratch
        case 0x341: csr_mepc = val; break;            // mepc
        case 0x342: csr_mcause = val; break;          // mcause
        case 0x343: csr_mtval = val; break;           // mtval
        case 0x344: csr_mip = val; break;             // mip
        // S-mode (sinks)
        case 0x180: csr_satp = val; break;            // satp (sink)
        // Read-only CSRs (misa, mhartid, counters) — silently ignore writes
        default: break;
    }
}

// ------------------------------------------------------------
// Stage: Fetch
// ------------------------------------------------------------
FetchOut fetch(volatile uint32_t* ram) {
    #pragma HLS INLINE
    FetchOut f;
    
    unsigned im_idx = addr_to_idx((unsigned)pc);
    
    #ifdef __SYNTHESIS__
        f.instr = (ap_uint<32>)ram[im_idx];
    #else
        if (im_idx < RAM_SIZE) {
            f.instr = (ap_uint<32>)ram[im_idx];
        } else {
            f.instr = 0; 
        }
    #endif
    f.pc = pc;

    if(CORE_DEBUG) {
        std::cout << "\n------------------------------------------------------------\n";
        std::cout << "[FETCH] PC=0x" << std::hex << (unsigned)pc 
                  << " Instr=0x" << (unsigned)f.instr << std::dec << "\n";
    }
    return f;
}

// ------------------------------------------------------------
// Stage: Decode
// ------------------------------------------------------------
DecodeOut decode(const FetchOut& f) {
    #pragma HLS INLINE
    DecodeOut d;
    ap_uint<32> instr = f.instr;

    d.opcode = instr.range(6, 0);
    d.rd     = instr.range(11, 7);
    d.funct3 = instr.range(14, 12);
    d.rs1    = instr.range(19, 15);
    d.rs2    = instr.range(24, 20);
    d.funct7 = instr.range(31, 25);
    d.instr  = instr;

    // Uniform Switch for Immediate Extraction
    switch (d.opcode) {
        case 0x23: d.imm = sextS(instr); break; // Store
        case 0x63: d.imm = sextB(instr); break; // Branch
        case 0x6F: d.imm = sextJ(instr); break; // JAL
        case 0x2F: d.imm = 0; break;            // Atomics (No immediate)
        default:   d.imm = sextI(instr); break; // All others (I-type, JALR, Load, ALU-I)
    }
    
    d.pc = f.pc;
    d.rs1_val = (d.rs1 == 0) ? (ap_int<32>)0 : regfile[d.rs1];
    d.rs2_val = (d.rs2 == 0) ? (ap_int<32>)0 : regfile[d.rs2];

    if (CORE_DEBUG) {
        std::cout << "[DECODE] Opcode=0x" << std::hex << (int)d.opcode 
                  << " Rd=" << (int)d.rd << std::dec << "\n";
    }
    return d;
}

// ------------------------------------------------------------
// Stage: Execute
// ------------------------------------------------------------
ExecOut execute(const DecodeOut& d) {
    #pragma HLS INLINE
    ap_int<32> rs1_val = d.rs1_val;
    ap_int<32> rs2_val = d.rs2_val;

    ExecOut e;
    e.alu_result   = 0;
    e.rd           = d.rd;
    e.mem_read     = false;
    e.mem_write    = false;
    e.reg_write    = false;
    e.store_val    = rs2_val;
    e.funct3       = d.funct3;
    e.is_trap      = false;
    e.is_atomic    = false;
    e.atomic_op    = 0;
    e.branch_taken = false;
    e.next_pc      = 0;
    e.finished     = false;

    switch ((unsigned)d.opcode) {
    case 0x2F: { // A-Extension (Atomics)
        if (ENABLE_A_EXTENSION) {
            if (d.funct3 == 0x2) {
                e.is_atomic  = true;
                e.atomic_op  = d.funct7.range(6, 2);
                e.alu_result = rs1_val; // Memory Address
                e.store_val  = rs2_val;
                e.reg_write  = true;
            } else {
                e.is_trap = true;
            }
        } else {
            e.is_trap = true;
        }
        break;
    }
    case 0x33: { // R-type
            ap_uint<5> shamt = rs2_val.range(4,0);
            e.reg_write = true;
            
            switch ((unsigned)d.funct7) {
                case 0x00:
                    switch ((unsigned)d.funct3) {
                        case 0x0: e.alu_result = rs1_val + rs2_val; break; // ADD
                        case 0x1: e.alu_result = rs1_val << shamt;  break; // SLL
                        case 0x2: e.alu_result = (rs1_val < rs2_val) ? 1 : 0; break; // SLT
                        case 0x3: e.alu_result = ((ap_uint<32>)rs1_val < (ap_uint<32>)rs2_val) ? 1 : 0; break; // SLTU
                        case 0x4: e.alu_result = rs1_val ^ rs2_val; break; // XOR
                        case 0x5: e.alu_result = (ap_uint<32>)rs1_val >> shamt; break; // SRL
                        case 0x6: e.alu_result = rs1_val | rs2_val; break; // OR
                        case 0x7: e.alu_result = rs1_val & rs2_val; break; // AND
                        default:  e.reg_write = false; e.is_trap = true; break; 
                    }
                    break;
                case 0x20:
                    switch ((unsigned)d.funct3) {
                        case 0x0: e.alu_result = rs1_val - rs2_val; break; // SUB
                        case 0x5: e.alu_result = rs1_val >> shamt; break; // SRA
                        default:  e.reg_write = false; e.is_trap = true; break;
                    }
                    break;
                case 0x01: // M-Extension (MUL, DIV, REM)
                    if (ENABLE_M_EXTENSION) {
                        bool is_div  = (d.funct3 == 0x4);
                        bool is_divu = (d.funct3 == 0x5);
                        bool is_rem  = (d.funct3 == 0x6);
                        bool is_remu = (d.funct3 == 0x7);
                        
                        bool is_div_op = is_div || is_divu || is_rem || is_remu;
                        bool is_signed = is_div || is_rem;

                        bool sign_a = rs1_val[31];
                        bool sign_b = rs2_val[31];
                        
                        ap_uint<32> u_a = (is_signed && sign_a) ? (ap_uint<32>)(-rs1_val) : (ap_uint<32>)rs1_val;
                        ap_uint<32> u_b = (is_signed && sign_b) ? (ap_uint<32>)(-rs2_val) : (ap_uint<32>)rs2_val;

                        ap_uint<32> u_quot = 0;
                        ap_uint<32> u_rem  = 0;

                        if (is_div_op) {
                            if (u_b != 0) {
                                u_quot = u_a / u_b;
                                u_rem = u_a - (u_quot * u_b);
                            } else {
                                u_quot = -1;
                                u_rem  = u_a;
                            }
                        }

                        ap_int<32> res_div = 0;
                        ap_int<32> res_rem = 0;

                        if (is_signed) {
                            res_div = ((sign_a ^ sign_b) && u_b != 0) ? (ap_int<32>)-u_quot : (ap_int<32>)u_quot;
                            res_rem = (sign_a && u_b != 0) ? (ap_int<32>)-u_rem : (ap_int<32>)u_rem;
                            
                            if (u_b == 1 && sign_b && !sign_a && rs1_val == (ap_int<32>)0x80000000) {
                                res_div = rs1_val;
                                res_rem = 0;
                            }
                        } else {
                            res_div = (ap_int<32>)u_quot;
                            res_rem = (ap_int<32>)u_rem;
                        }

                        switch ((unsigned)d.funct3) {
                            case 0x0: e.alu_result = rs1_val * rs2_val; break; // MUL
                            case 0x1: e.alu_result = (ap_int<32>)(((ap_int<64>)rs1_val * (ap_int<64>)rs2_val) >> 32); break; // MULH
                            case 0x2: e.alu_result = (ap_int<32>)(((ap_int<64>)rs1_val * (ap_uint<64>)((ap_uint<32>)rs2_val)) >> 32); break; // MULHSU
                            case 0x3: e.alu_result = (ap_int<32>)(((ap_uint<64>)((ap_uint<32>)rs1_val) * (ap_uint<64>)((ap_uint<32>)rs2_val)) >> 32); break; // MULHU
                            case 0x4: e.alu_result = res_div; break; // DIV
                            case 0x5: e.alu_result = res_div; break; // DIVU
                            case 0x6: e.alu_result = res_rem; break; // REM
                            case 0x7: e.alu_result = res_rem; break; // REMU
                        }
                    } else {
                        e.reg_write = false; 
                        e.is_trap = true; 
                    }
                    break;
                default:
                    e.reg_write = false; 
                    e.is_trap = true; 
                    break;
            }
            break;
    }
    case 0x17: { // AUIPC
            ap_int<32> imm20 = (ap_int<32>)(d.instr & 0xFFFFF000);
            e.alu_result = (ap_int<32>)d.pc + imm20;
            e.reg_write  = (d.rd != 0);
            break;
    }
    case 0x13: { // I-type
            if (d.rd == 0 && d.rs1 == 0 && d.imm == 0) { // NOP
                e.reg_write = false;
            } else {
                e.reg_write = true; 
            }
            ap_uint<5> shamt = d.imm.range(4,0); 
            
            switch ((unsigned)d.funct3) {
                case 0x0: e.alu_result = rs1_val + d.imm; break; // ADDI
                case 0x1: e.alu_result = rs1_val << shamt;  break; // SLLI
                case 0x2: e.alu_result = (rs1_val < d.imm) ? 1 : 0; break; // SLTI
                case 0x3: e.alu_result = ((ap_uint<32>)rs1_val < (ap_uint<32>)d.imm) ? 1 : 0; break; // SLTIU
                case 0x4: e.alu_result = rs1_val ^ d.imm; break; // XORI
                case 0x5: 
                    if (d.instr[30] == 0) e.alu_result = (ap_uint<32>)rs1_val >> shamt; // SRLI
                    else e.alu_result = rs1_val >> shamt; // SRAI
                    break;
                case 0x6: e.alu_result = rs1_val | d.imm; break; // ORI
                case 0x7: e.alu_result = rs1_val & d.imm; break; // ANDI
                default: e.reg_write = false; e.is_trap = true; break;
            }
            break;
    }
    case 0x03: { // Load
            e.alu_result = rs1_val + d.imm;
            e.mem_read   = true;
            e.reg_write  = true;
            break;
    }
    case 0x23: { // Store
            e.alu_result = rs1_val + d.imm;
            e.mem_write  = true;
            break;
    }
    case 0x63: { // Branch
            ap_int<32> tgt = (ap_int<32>)d.pc + d.imm;
            bool taken = false;
            
            switch ((unsigned)d.funct3) {
                case 0x0: taken = (rs1_val == rs2_val); break; // BEQ
                case 0x1: taken = (rs1_val != rs2_val); break; // BNE
                case 0x4: taken = (rs1_val < rs2_val);  break; // BLT
                case 0x5: taken = (rs1_val >= rs2_val); break; // BGE
                case 0x6: taken = ((ap_uint<32>)rs1_val < (ap_uint<32>)rs2_val);  break; // BLTU
                case 0x7: taken = ((ap_uint<32>)rs1_val >= (ap_uint<32>)rs2_val); break; // BGEU
                default:  taken = false; break;
            }

            if(taken) {
                e.next_pc = (ap_uint<32>)tgt;
                e.branch_taken = true;
            }
            break;
    }
    case 0x6F: { // JAL
            e.alu_result    = (ap_int<32>)(d.pc + 4);
            e.next_pc       = (ap_uint<32>)((ap_int<32>)d.pc + d.imm);
            e.branch_taken  = true;
            e.reg_write     = (d.rd != 0);
            break;
    }
    case 0x67: { // JALR
            e.alu_result    = (ap_int<32>)(d.pc + 4); 
            e.next_pc       = (ap_uint<32>)(((ap_int<32>)rs1_val + d.imm) & (~1));
            e.branch_taken  = true;
            e.reg_write     = (d.rd != 0);
            break;
    }
    case 0x37:{ // LUI
            e.alu_result = (ap_int<32>)(d.instr & 0xFFFFF000);
            e.reg_write  = (d.rd != 0);
            break;
    }
    case 0x73: { // System
        unsigned csr_addr = d.imm.range(11,0); 
        ap_uint<5> rs1_imm = d.rs1; 
        ap_uint<32> trap_cause = 0;
        
        ap_uint<32> csr_read_val = csr_read(csr_addr);
        
        e.reg_write = false; 

        switch ((unsigned)d.funct3) {
            case 0x0: // ECALL / EBREAK / WFI / MRET
                if (d.imm == 0x000) { 
                    if (regfile[17] == 93) {
                        e.finished = true;
                        #ifndef __SYNTHESIS__
                        std::cout << "[CORE DEBUG] Exit Condition Met! Stopping Simulation." << std::endl;
                        #endif
                    }
                    e.is_trap = true; 
                    trap_cause = 11; 
                } // ECALL
                else if (d.imm == 0x001) { e.is_trap = true; trap_cause = 3; } // EBREAK
                else if (d.imm == 0x105) { // WFI (Wait For Interrupt)
                    #ifndef __SYNTHESIS__
                    bool global_enable = (csr_mstatus >> 3) & 1;
                    bool timer_enable  = (csr_mie >> 7) & 1;

                    if (csr_mcycle > 500000) { 
                        mtimecmp = csr_mcycle + 100;
                        
                        std::cout << "[SIM-HACK] WFI at Cycle " << std::hex << (uint64_t)csr_mcycle 
                                  << " | MIE (Global): " << (int)global_enable 
                                  << " | MTIE (Timer): " << (int)timer_enable 
                                  << std::dec << std::endl << std::flush;
                    }
                    #endif
                }
                else if (d.imm == 0x302) { // MRET
                    e.next_pc = (ap_uint<32>)csr_mepc;
                    e.branch_taken = true;

                    bool mpie = (csr_mstatus >> 7) & 1;
                    if(mpie) csr_mstatus |= (1 << 3);
                    else     csr_mstatus &= ~(1 << 3);
                    csr_mstatus |= (1 << 7);

                    e.reg_write = false; 
                    if(CORE_DEBUG) std::cout << "[MRET] Returning to 0x" << std::hex << (int)e.next_pc << std::dec << "\n";
                }
                break;
            case 0x1: // CSRRW
                e.alu_result = csr_read_val; 
                e.reg_write = (d.rd != 0);
                csr_write(csr_addr, (ap_uint<32>)rs1_val);
                break;
            case 0x2: // CSRRS
                e.alu_result = csr_read_val; 
                e.reg_write = (d.rd != 0);
                if (d.rs1 != 0)
                    csr_write(csr_addr, csr_read_val | (ap_uint<32>)rs1_val);
                break;
            case 0x3: // CSRRC
                e.alu_result = csr_read_val;
                e.reg_write = (d.rd != 0);
                if (d.rs1 != 0)
                    csr_write(csr_addr, csr_read_val & ~(ap_uint<32>)rs1_val);
                break;
            case 0x5: // CSRRWI
                e.alu_result = csr_read_val; 
                e.reg_write = (d.rd != 0);
                csr_write(csr_addr, (ap_uint<32>)rs1_imm);
                break;
            case 0x6: // CSRRSI
                e.alu_result = csr_read_val;
                e.reg_write = (d.rd != 0);
                if (rs1_imm != 0)
                    csr_write(csr_addr, csr_read_val | (ap_uint<32>)rs1_imm);
                break;
            case 0x7: // CSRRCI
                e.alu_result = csr_read_val;
                e.reg_write = (d.rd != 0);
                if (rs1_imm != 0)
                    csr_write(csr_addr, csr_read_val & ~(ap_uint<32>)rs1_imm);
                break;
            default:
                e.is_trap = true;
                trap_cause = 2; 
                break;
        }

        if (e.is_trap) {
            csr_mepc = d.pc;       
            csr_mcause = trap_cause;  
            e.next_pc = csr_mtvec;      
            e.branch_taken = true; 
            e.reg_write = false; 
        }
        break;
    } 
    case 0x0F: { // FENCE and FENCE.I
        e.reg_write = false; 

        switch ((unsigned)d.funct3) {
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
            e.is_trap = true;
            csr_mepc = d.pc;       
            csr_mcause = 2; 
            e.next_pc = csr_mtvec;     
            e.branch_taken = true; 
            e.reg_write = false; 
            break;
    }

    if (CORE_DEBUG) {
        std::cout << "[EXEC] ALU=0x" << std::hex << (int)e.alu_result << std::dec << "\n";
    }
    return e;
}

// ------------------------------------------------------------
// Stage: Memory
// ------------------------------------------------------------
MemOut memory(volatile uint32_t* ram, const ExecOut& e) {
    #pragma HLS INLINE
    MemOut m;
    m.is_trap = e.is_trap;

    // Local copies that may be suppressed by trap
    bool mem_read  = e.mem_read;
    bool mem_write = e.mem_write;
    bool reg_write = e.reg_write;

    if (e.is_trap) {
        mem_read  = false;
        mem_write = false;
        reg_write = false; 
    }

    m.value     = e.alu_result;  
    m.rd        = e.rd;
    m.reg_write = reg_write;

    unsigned ea_u    = (unsigned)e.alu_result; 
    unsigned phys_ea = ea_u & 0x07FFFFFF;        // Used for CLINT MMIO checks
    unsigned d_idx   = addr_to_idx(ea_u);        // Synthesis: ea_u>>2, Sim: array-relative
    unsigned byte_off = ea_u & 0x3;

    // =============================================================
    // ATOMIC MEMORY OPERATIONS (A-EXTENSION)
    // =============================================================
    if (e.is_atomic) {
        #ifndef __SYNTHESIS__
        if (d_idx >= RAM_SIZE) { 
            m.value = 0; // Fault
        } else 
        #endif
        {
            uint32_t loaded_raw = ram[d_idx];
            ap_int<32> loaded_val = (ap_int<32>)loaded_raw;
            ap_int<32> write_val = 0;
            bool do_write = false;

            // Load Reserved (LR.W)
            if (e.atomic_op == 0x02) { 
                lr_addr = ea_u;
                lr_valid = true;
                m.value = loaded_val;
                do_write = false;
                if(CORE_DEBUG) std::cout << "[AMO] LR at 0x" << std::hex << ea_u << std::dec << "\n";
            } 
            // Store Conditional (SC.W)
            else if (e.atomic_op == 0x03) {
                if (lr_valid && lr_addr == ea_u) {
                    write_val = e.store_val;
                    do_write = true;
                    m.value = 0; // Success
                    lr_valid = false;
                } else {
                    do_write = false;
                    m.value = 1; // Failure
                }
                if(CORE_DEBUG) std::cout << "[AMO] SC at 0x" << std::hex << ea_u << (do_write ? " Success" : " Fail") << std::dec << "\n";
            }
            // AMO Read-Modify-Write Operations
            else {
                ap_int<32> op_b = e.store_val;
                do_write = true;
                switch (e.atomic_op) {
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
                m.value = loaded_val; // AMOs write original value to Rd
            }

            if (do_write) {
                ram[d_idx] = (uint32_t)write_val;
                lr_valid = false; 
            }
        }
    }
    // =============================================================
    // NORMAL LOAD
    // =============================================================
    else if (mem_read) {
        // ----------------------------------------------------------------
        // MMIO READ: UART
        // ----------------------------------------------------------------
        if ((ea_u & 0xFFFFF000) == 0x10000000) {
            #ifdef __SYNTHESIS__
                m.value = (ap_int<32>)ram[d_idx]; // Read from real UART via AXI
            #else
                m.value = 0x60; // Sim: always "TX Ready"
            #endif
            m.reg_write = true;
            return m;
        }
        
        // ----------------------------------------------------------------
        // MMIO: CLINT (emulated internally)
        // ----------------------------------------------------------------
        // mtimecmp (0x2004000) - Timer Compare Register
        if (phys_ea == 0x2004000) {
            m.value = (ap_int<32>)(mtimecmp & 0xFFFFFFFF);
            m.reg_write = true;
            return m;
        }
        if (phys_ea == 0x2004004) {
            m.value = (ap_int<32>)(mtimecmp >> 32);
            m.reg_write = true;
            return m;
        }
        
        // mtime (0x200BFF8) - Current Time (Aliased to csr_mcycle)
        if (phys_ea == 0x200BFF8) {
            m.value = (ap_int<32>)(csr_mcycle & 0xFFFFFFFF);
            m.reg_write = true;
            return m;
        }
        if (phys_ea == 0x200BFFC) {
            m.value = (ap_int<32>)(ap_uint<32>)(csr_mcycle >> 32);
            m.reg_write = true;
            return m;
        }

        // ----------------------------------------------------------------
        // STANDARD RAM READ
        // ----------------------------------------------------------------
        #ifndef __SYNTHESIS__
        if (d_idx >= RAM_SIZE) {
            if(CORE_DEBUG) std::cerr << "[MEM] Load OOB: EA=0x" << std::hex << ea_u << "\n";
            m.value = 0;
        } else 
        #endif
        {
            uint32_t raw_w0 = ram[d_idx];
            uint32_t raw_w1 = (d_idx + 1 < RAM_SIZE) ? (uint32_t)ram[d_idx + 1] : 0;

            ap_uint<32> word0 = (ap_uint<32>)raw_w0;
            ap_uint<32> word1 = (ap_uint<32>)raw_w1;
            
            ap_uint<32> raw_val_32 = (word0 >> (byte_off * 8)) | (word1 << ((4 - byte_off) * 8));
            ap_int<32> loaded_val = 0;
            m.reg_write = true;

            switch ((unsigned)e.funct3) { 
                case 0: loaded_val = (ap_int<32>)((ap_int<8>)raw_val_32.range(7, 0)); break; // LB
                case 1: loaded_val = (ap_int<32>)((ap_int<16>)raw_val_32.range(15, 0)); break; // LH
                case 2: loaded_val = (ap_int<32>)raw_val_32; break; // LW
                case 4: loaded_val = (ap_int<32>)((ap_uint<32>)raw_val_32.range(7, 0)); break; // LBU
                case 5: loaded_val = (ap_int<32>)((ap_uint<32>)raw_val_32.range(15, 0)); break; // LHU
                default: m.reg_write = false; break; 
            }
            m.value = loaded_val; 
        }
    }
    // =============================================================
    // NORMAL STORE
    // =============================================================
    else if (mem_write) {
        // Any standard write invalidates a Load Reservation
        lr_valid = false;
        
        // ----------------------------------------------------------------
        // MMIO: UART Write
        // ----------------------------------------------------------------
        if ((ea_u & 0xFFFFF000) == 0x10000000) { 
            #ifdef __SYNTHESIS__
                // Hardware: Direct word write to UART via AXI (no read-modify-write!)
                ram[d_idx] = (uint32_t)(ap_uint<32>)e.store_val;
            #else
                // Simulation: Print character
                if ((ea_u & 0xFF) == 0x00) {
                    char c = (char)(e.store_val & 0xFF);
                    std::cout << c << std::flush; 
                }
            #endif
            m.reg_write = false; 
            return m;
        }

        // ----------------------------------------------------------------
        // MMIO: CLINT (Timer Compare)
        // ----------------------------------------------------------------
        if (phys_ea == 0x2004000) {
            mtimecmp = (mtimecmp & 0xFFFFFFFF00000000) | (ap_uint<32>)e.store_val;
            if(CORE_DEBUG) std::cout << "[CLINT] mtimecmp Low Update: " << std::hex << mtimecmp << std::dec << "\n";
            return m;
        }
        if (phys_ea == 0x2004004) {
             mtimecmp = (mtimecmp & 0x00000000FFFFFFFF) | ((ap_uint<64>)e.store_val << 32);
             if(CORE_DEBUG) std::cout << "[CLINT] mtimecmp High Update: " << std::hex << mtimecmp << std::dec << "\n";
             return m;
        }

        // ----------------------------------------------------------------
        // STANDARD RAM STORE
        // ----------------------------------------------------------------
        #ifndef __SYNTHESIS__
        if (d_idx >= RAM_SIZE) {
            if(CORE_DEBUG) std::cerr << "[MEM] Store OOB: EA=0x" << std::hex << ea_u << "\n";
        } else 
        #endif
        {
            // Read-Modify-Write
            uint32_t raw_w0 = ram[d_idx];
            ap_uint<32> word0 = (ap_uint<32>)raw_w0;
            
            ap_uint<32> store_val = (ap_uint<32>)e.store_val;
            ap_uint<32> mask0 = 0;
            ap_uint<32> mask1 = 0;
            
            switch ((unsigned)e.funct3) {
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
            ram[d_idx] = (uint32_t)word0;

            // Modify Word 1 (Boundary Crossing)
            if (mask1 != 0 && d_idx + 1 < RAM_SIZE) {
                uint32_t raw_w1 = ram[d_idx + 1];
                ap_uint<32> word1 = (ap_uint<32>)raw_w1;
                
                word1 = (word1 & ~mask1) | ((store_val >> ((4-byte_off)*8)) & mask1);
                ram[d_idx + 1] = (uint32_t)word1; 
            }
            
            // ----------------------------------------------------------------
            // HTIF INTERCEPTOR (Prevents Syscall Deadlock - Sim Only)
            // ----------------------------------------------------------------
            #ifndef __SYNTHESIS__
            if (phys_ea == 0x1000) {
                unsigned fromhost_idx = d_idx + 16; 
                if (fromhost_idx < RAM_SIZE) {
                    ram[fromhost_idx] = 1; 
                }
            }
            #endif
            // ----------------------------------------------------------------
            
            if(CORE_DEBUG) std::cout << "[MEM] Stored 0x" << std::hex << (int)e.store_val << " to 0x" << ea_u << std::dec << "\n";
        }
    }
    return m;
}

// ------------------------------------------------------------
// Stage: Writeback
// ------------------------------------------------------------
void writeback(const MemOut& m) {
    #pragma HLS INLINE
    if (m.reg_write && m.rd != 0 && !m.is_trap) {
        regfile[m.rd] = m.value;
        if(CORE_DEBUG) std::cout << "[WB] x" << (int)m.rd << " <= 0x" << std::hex << (int)m.value << std::dec << "\n";
    }
    regfile[0] = 0; 
}

// ------------------------------------------------------------
// Top-Level Step Function
// ------------------------------------------------------------
void riscv_step(volatile uint32_t* ram, int max_cycles, int* cycles_output) {
    // In hardware, driver must set m_axi base address to 0x0 so the core can
    // address both DDR (0x80000000) and UART (0x10000000) via SmartConnect routing.
    #pragma HLS INTERFACE m_axi port=ram offset=off depth=262144 bundle=gmem
    // Control Parameters
    #pragma HLS INTERFACE s_axilite port=max_cycles bundle=control
    #pragma HLS INTERFACE s_axilite port=cycles_output bundle=control
    // AXI Lite Interface for Control
    #pragma HLS INTERFACE s_axilite port=return bundle=control
    #pragma HLS BIND_STORAGE variable=regfile type=ram_2p impl=lutram

    // =========================================================
    #ifdef __SYNTHESIS__
        pc = 0x80000000; // Hardcode the default boot address for the FPGA
        for(int i=0; i<32; i++) regfile[i] = 0;
        
        // Setup default stack pointer for hardware (e.g., 64MB into DDR)
        regfile[2] = 0x80000000 + 0x4000000; 

        // Reset CSRs
        csr_mcycle = 0;
        csr_minstret = 0;
        csr_mstatus = 0;
        csr_mie = 0;
        csr_mip = 0;
        mtimecmp = 0xFFFFFFFFFFFFFFFF;
        lr_valid = false;
    #endif
    // =========================================================

    is_finished = false;

    INSTRUCTION_LOOP: while(true) {
        #pragma HLS LOOP_TRIPCOUNT min=50 max=500000

        // ------------------ Cycle Counter ------------------
        csr_mcycle++;

        // --- HEARTBEAT ---
        if (csr_mcycle % 1000000 == 0) {
            #ifdef __SYNTHESIS__
            // Do nothing
            #else
            std::cout << "Cycle: " << std::dec << (uint64_t)csr_mcycle 
                      << " | PC: 0x" << std::hex << (unsigned)pc << std::endl;
            #endif
        }

        // ------------------ INTERRUPT LOGIC  ------------------
        bool timer_irq = (csr_mcycle >= mtimecmp);

        bool global_ie = (csr_mstatus >> 3) & 1;
        bool timer_ie  = (csr_mie >> 7) & 1;

        if (timer_irq) csr_mip |= (1 << 7);
        else           csr_mip &= ~(1 << 7);

        if (timer_irq && global_ie && timer_ie) {
            if (CORE_DEBUG) std::cout << "[INT] Timer Interrupt! Jumping to Handler.\n";
            
            csr_mcause = 0x80000007;
            csr_mepc   = pc;
            
            bool old_mie = (csr_mstatus >> 3) & 1;
            if (old_mie) csr_mstatus |= (1 << 7);
            else         csr_mstatus &= ~(1 << 7);
            
            csr_mstatus &= ~(1 << 3);
            pc = csr_mtvec; 
            continue; 
        }

        // ------------------ Execute Pipeline ------------------
        FetchOut  f = fetch(ram);
        DecodeOut d = decode(f);
        ExecOut   e = execute(d);
        MemOut    m = memory(ram, e);
        writeback(m);

        // Instruction retired
        csr_minstret++;

        // ========== NEXT PC ==========
        if (e.branch_taken) {
            pc = e.next_pc;
        } else {
            pc += 4;
        }

        // Break loop if ecall exit or cycle limit reached (0 = run forever)
        if (e.finished || (max_cycles > 0 && (int)(ap_uint<32>)csr_mcycle >= max_cycles)) {
            *cycles_output = (int)(ap_uint<32>)csr_mcycle;
            return;
        }
    }
}