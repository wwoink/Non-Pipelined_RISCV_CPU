## Running the Project in Vitis HLS (UI Workflow)

---

### 1) Open the Project

Launch **Vitis HLS**.

From the welcome screen, set the workspace:  
This is just the repo **Processor_From_Scratch-main**

---

### 2) Verify Source Files

In the **Sources** view, confirm these are added:

- `src/core.cpp`  
- Your testbench (e.g., `Testbench_elf_batch.cpp` or `Testbench_elf` )

If anything is missing, **Add Sources** (right-click **Sources → Add Files…**).

---

### 3) C Simulation Settings

**Program Arguments (important):** add your test path(s).  
(This should be saved in `hls_config.cfg` but sometimes the path doesn't like to show up automatically.  
You can copy it from the config underneath the **C Simulation** section if it doesn't show up after a couple times of clicking run.)
(Here it is as well) `../../../../Benchmarks/rv32ui-p-benchmarks`

The Path is different depending on the testbench. If you are using the batch version then it simply needs to be passed the file like in the example above. If you are looking at an indivdual test using the other testbench
then it needs to be in the path as well (ex: `../../../../Benchmarks/rv32ui-p-benchmarks/rsort.riscv`)

Make sure that in the config file under General/C Synthesis sources the CFLAGS, CSIMFLAGS have the argument: -I ./include
Aso put this under the C testbench CFLAGS section (These should be here from the config but just in case they aren't)

---

### 4) Run C Simulation

In the **Flow** or **Flow Navigator** panel, click **C Simulation ▶ Run**.

If your arguments are correct, the tests will execute and print **SUMMARY: _ Passed, _ Failed** lines from the testbench.  
For the Bath verison we can see each test and if it passed or failed. If it failed then the error code will be next to it.
Some tests timeout. This is becuae they do not finish in the alloted cycles. This is an issue similar to failure that needs
to be looked into more.

The regular testbench is similar and will also print PASS or FAIL with the error code.

---
