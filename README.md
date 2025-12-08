## Running the Project in Vitis HLS (UI Workflow)

---

### 1) Open the Project

Launch **Vitis HLS**.

From the welcome screen, set the workspace:  
This is just the repo **Non-Pipelined_RISCV_CPU**

---

### 2) Verify Source Files

In the **Sources** view, confirm these are added:

- `src/core.cpp`
  
In **Test Bench**:

- Your testbench (e.g., `Testbench_elf_batch.cpp`, `Testbench_elf`, or `Testbench_HardCoded` )

If anything is missing, **Add Sources** (right-click **Sources → Add Files…**).

---

### 3) C Simulation Settings

**Program Arguments (important):** add your test path(s).  
(This should be saved in `hls_config.cfg` but sometimes the path doesn't like to show up automatically.  
You can copy it from the config underneath the **C Simulation** section if it doesn't show up after a couple times of clicking run.)
(Here it is as well for the batch) `../../../../Benchmarks/rv32ui-p-benchmarks`

The Path is different depending on the testbench. If you are using the batch version then it simply needs to be passed the file like in the example above. If you are looking at an indivdual test using the other testbench
then it needs to be in the path as well (ex: `../../../../Benchmarks/rv32ui-p-benchmarks/rsort.riscv`). There is no argument for the hard coded test bench. This means to run a different test you need to type in the path
inside of the testbench. It needs to be an absolute path that points to the benchmark similar to the normal version. This is used in cosim to avoid any issues with relative paths as cosim will run in a differnt directory.

Make sure that in the config file under General/C Synthesis sources the CFLAGS, CSIMFLAGS have the argument: -I ./include
Aso put this under the C testbench CFLAGS section (These should be here from the config but just in case they aren't)

---

### 4) Run C Simulation

In the **Flow** or **Flow Navigator** panel, click **C Simulation ▶ Run**.

If your arguments are correct, the tests will execute and print **SUMMARY: _ Passed, _ Failed** lines from the testbench.  
For the Bath verison we can see each test and if it passed or failed. If it failed then the error code will be next to it.
Some tests timeout. This is becuae they do not finish in the alloted cycles. This would be an equivolent of failing.

The regular and hardcoded testbenchs are similar and will also print PASS or FAIL with the error code.

---

### 5) Run C Synthesis

No matter what testbench is currently used C synthesis will run.

In the **Flow** or **Flow Navigator** panel, click **C Synthesis ▶ Run**.

No arguments need to be changed here. The default of 5ns and the exact FPGA are fine.
This will generate the actual VHDL and Verilog files. These can be found in the path `Non-Pipelined_RISCV_CPU\Global_Core_Revised\Global_Core_Revised\hls\syn`. Here there are 3 folders. 1 with the VHDL, 1 With the Verilog, and 1 containing all of the logs.

---


### 6) Run C/RTL COSIM

This is **only** ment to be run with the hardcoded benchmark. This is to do with the need for an abosolute path as the files are located in a different spot than the argument used previously.
It is easy to change the abosolute path in the testbench to test other benchmarks.

In the **Flow** or **Flow Navigator** panel, click **C/RTL COSIMULATION ▶ Run**.

This will take a few minutes to run. It will also use about 20GB of space for files generated during the test. Previously when using 1 function call in the testbench, this step was generating hundreds more GB of test files.
With the current batch runner it is significantly faster and much more space efficient.

When it Finishs you will see a pass or fail at the end. This represents if it matches the C Sim data tested. So far the Qsort and Rsort have been tested and pass.
This is a good sign as all of the other tests pass the CSIM and these tests are alreay passing COSIM.

---


## 7) Package and Implementation

These do run but I have not had time to look into them deeper. 
