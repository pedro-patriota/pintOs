# PintOS Setup and Debugging Guide

This document describes how to run PintOS for the first time, including setup instructions, compilation, and debugging approaches using DDD, Eclipse, and Visual Studio Code (VSCode).

## Additional Resources

Before getting started, you can access additional documentation at:
- [Extended Setup and Configuration Guide](https://docs.google.com/document/d/1YqSes5JwS96W0yLYuSyUg-R9lmdQHzOrCPQW0OHPvZQ/edit?tab=t.0)
- [PintOS Source Code](https://drive.google.com/file/d/1xCZIVAZ-Y7Lj2udkA9CEG2So0EpLh4Ct/view)
- [Official PintOS Documentation](https://www.scs.stanford.edu/23wi-cs212/pintos/pintos_1.html)

---

## I. Initial Setup and Running PintOS (Tested on Ubuntu 16 and 22)

### Prerequisites

1. **Download the PintOS source code** (`pintos-linux.zip`) from the Google Drive link above

2. **Install QEMU** (the simulator for running PintOS):
   ```bash
   sudo apt install qemu-system-i386
   ```

### Step-by-Step Setup

#### Step 1: Build the Core Components
Navigate to the pintos directory and compile the essential components:
```bash
cd pintos/src/utils
make

cd ../threads
make
```
**Important**: Verify successful compilation before running tests.

#### Step 2: Configure PATH Environment Variable
Edit your `.bashrc` file (access it by showing hidden files in your home directory):
```bash
nano ~/.bashrc
```
Add the following line at the end:
```bash
export PATH=$PATH:/home/your_username/pintos/src/utils
```
Then reload the configuration:
```bash
source ~/.bashrc
```

#### Step 3: Configure PintOS to Use QEMU
Navigate to `pintos/src/threads` and open `Make.vars`:
```bash
cd pintos/src/threads
nano Make.vars
```
Find the line:
```makefile
SIMULATOR = --bochs
```
Change it to:
```makefile
SIMULATOR = --qemu
```

### Running Tests

#### Run a Single Test
To run a specific test (e.g., `alarm-multiple`):
```bash
cd pintos/src/threads
pintos --qemu -- run alarm-multiple
```

You can find the available test names and their descriptions in `pintos/src/tests/threads/` directory.

#### Run All Tests
To compile and run all tests at once:
```bash
cd pintos/src/threads/build
make check
```
**Note**: Priority scheduling tests are not counted in the automated checks.

### Troubleshooting

#### Virtual Machine Sudoers Error
If you're using a VM and encounter the error: `"vboxusers is not in the sudoers files"`

1. Open a terminal and type:
   ```bash
   su
   ```
2. Enter your password
3. Edit the sudoers file:
   ```bash
   sudo nano /etc/sudoers
   ```
4. Find the line:
   ```
   %sudo ALL=(ALL:ALL) ALL
   ```
5. Add your username below it:
   ```
   your_username ALL=(ALL) ALL
   ```
6. Press `Ctrl+O`, then `Enter`, and close the terminal

#### Floating-Point Operations
PintOS does not support conventional floating-point operations. Use the definitions from the `float.h` file (included in this repository) for all operations that use or result in floating-point numbers.

---

## II. Debugging with DDD (Data Display Debugger - GDB GUI)

DDD provides a graphical interface to the GNU Debugger (GDB), making it easier to debug PintOS code.

**Additional Debugging Methods**: See the [PintOS debugging documentation](https://www.scs.stanford.edu/23wi-cs212/pintos/pintos_10.html#SEC145) for other approaches including `printf` debugging.

### Installation and Setup

1. **Install DDD**:
   ```bash
   sudo apt install ddd
   ```

2. **Create DDD wrapper script**:
   ```bash
   cd pintos/src/utils
   cp pintos-gdb ddd-pintos-gdb
   ```

3. **Edit the `ddd-pintos-gdb` script**:
   ```bash
   nano ddd-pintos-gdb
   ```
   - Find and update the `GDBMACROS` variable to the correct path where PintOS is installed on your machine:
     ```bash
     GDBMACROS=/home/your_username/pintos/src/misc/gdb-macros
     ```
   - Replace all occurrences of `GDB=gdb` with:
     ```bash
     GDB=ddd
     ```

4. **Compile the kernel**:
   ```bash
   cd pintos/src/threads
   make
   ```

### Debugging Workflow

You will need **two separate terminals**.

**Terminal 1** - Start PintOS in debug mode:
```bash
cd pintos/src/threads
../utils/pintos --qemu -v --gdb -- run alarm-multiple
```
This starts PintOS but pauses it, waiting for a debugger connection.

**Terminal 2** - Launch the debugger:
```bash
cd pintos/src/threads/build
../../utils/ddd-pintos-gdb kernel.o
```

### Using the DDD Debugger

1. **Open a source file**:
   - By default, `src/threads/init.c` opens
   - To debug a different file (e.g., `thread.c`), go to: `File → Open Source` and select your file

2. **Connect to PintOS**:
   - In the DDD console (bottom window), type:
     ```
     debugpintos
     ```
   - DDD/GDB will establish a connection with the running PintOS instance

3. **Set breakpoints**:
   - Right-click on the line of code where you want to pause
   - The execution will pause at that breakpoint

4. **Execute and navigate**:
   - `continue` - Resume execution until the next breakpoint
   - `step` - Execute the next line of code
   - Use the toolbar buttons for other debugging operations

### Troubleshooting DDD

If DDD freezes while continuing or running:
1. Press `Ctrl+C` to interrupt
2. Type `bt` (backtrace) in the DDD console to see the call stack and error location

The backtrace will show you exactly where an error occurred in the code.

---

## III. Debugging with Eclipse

This section covers setting up and debugging PintOS using Eclipse IDE, based on the [UChicago guide](https://uchicago-cs.github.io/mpcs52030/pintos_eclipse.html).

### Prerequisites

Download and install **Eclipse IDE for C/C++ Developers**:
- Download from: https://www.eclipse.org/downloads/packages/
- Tested with version 2023-06 (4.28.0)

### Creating an Eclipse Project

1. Go to: `File → Import → C/C++ → Existing Code as Makefile Project`

2. In the dialog, enter:
   - **Project Name**: `pintos`
   - **Existing Code Location**: Select the `pintos/src` directory (the directory containing `threads`, `tests`, `utils`, etc.)
   - **Languages**: Check `C` only; uncheck `C++`
   - **Toolchain for Indexer Settings**: Select `Cross GCC`

3. Click **Finish** to create the project

### Creating Build Configurations

This step is optional if you prefer compiling via the command line with `make`.

1. Right-click on the `pintos` project in the Project Explorer (left panel)

2. Go to: `Project → Properties → C/C++ Build`

3. Click **Manage Configurations...** then **New...**

4. In the dialog:
   - **Name**: Enter `pintos-thread`
   - **Description**: Leave blank
   - Click **OK**

5. Back in "Manage Configurations":
   - Select `pintos-thread`
   - Click **Set Active**

6. In the "C/C++ Build" window:
   - From the "Configuration" dropdown, select `pintos-thread`
   - Under "Build location", click **Workspace...** and select the `threads` directory
   - The path should look like: `${workspace_loc:/pintos/threads}`
   - Click **Apply and Close**

### Compiling the Project

- Use: `Project → Build All` or press `Ctrl+B`

**Note**: Eclipse may show a compilation error related to the linker (`ld`) using an obsolete command. This is a warning from `make` and can be safely ignored.

### Creating Debug Configurations

1. In the Project Explorer, navigate to: `threads/build/kernel.o`

2. Right-click on `kernel.o` and select: `Debug As → Debug Configurations...`

3. In the Debug Configurations window:
   - Double-click **GDB Hardware Debugging** to create a new configuration
   - Change the **Name** to: `pintos-thread`

4. Go to the **Main** tab:
   - **Build (if required) before launching**: Select `disable auto build`

5. Go to the **Debugger** tab:
   - **GDB debugger**: Change to:
     ```
     gdb -x /home/your_username/pintos/src/misc/gdb-macros
     ```
   - Under **Remote Target**:
     - Check **Use remote target**
     - **Debug server**: Select `Generic TCP/IP`
     - **Protocol**: Select `remote`
     - **Connection**: Enter `localhost:1234`

6. Go to the **Startup** tab:
   - Uncheck **Load Image**
   - Check **Load symbols**
   - Verify **Use project binary** shows: `.../threads/build/kernel.o`

### Debugging Workflow

**Before each debugging session**, compile the project using `Project → Build All`.

1. **Start PintOS in debug mode** (in a separate terminal):
   ```bash
   cd pintos/src/threads
   ../utils/pintos --qemu -v --gdb -- run alarm-multiple
   ```

2. **Set breakpoints in Eclipse**:
   - Open a source file (e.g., `thread.c`)
   - Double-click on the line number where you want to pause
   - For example, double-click the line containing `ASSERT(function != NULL)`

3. **Start debugging**:
   - In Eclipse's toolbar, click the dropdown arrow next to the bug icon and select `pintos-thread`
   - If you see "Errors exist in the active configuration...", click **Proceed**
   - Click the **Resume** button (the play icon with a vertical bar)
   - Use **Step** buttons to navigate through code

---

## IV. Debugging with Visual Studio Code (VSCode)

VSCode provides excellent C/C++ support with IntelliSense and integrated debugging capabilities.

### Installation and Setup

1. **Install Visual Studio Code**:
   - Download from: https://code.visualstudio.com/docs/setup/linux
   - Tested with version 1.100.3

2. **Install the C/C++ extension**:
   - Search for and install: **C/C++ IntelliSense, debugging, and code browsing**

3. **Open the PintOS source folder**:
   - Go to: `File → Open Folder`
   - Select the `pintos/src` directory

4. **Compile the kernel**:
   - Open the integrated terminal in VSCode
   - Navigate to the threads directory and compile:
     ```bash
     cd threads
     make
     ```

### Configuring the Debugger

1. Go to: `Run → Add Configurations` (or click on the `.vscode/launch.json` file)

2. Add the following configuration to your `launch.json`:

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "gdb pintos",
            "type": "cppdbg",
            "request": "launch",
            "MIMode": "gdb",
            "program": "${workspaceFolder}/threads/build/kernel.o",
            "miDebuggerPath": "/usr/bin/gdb",
            "miDebuggerArgs": "-s ${workspaceFolder}/threads/build/kernel.o -x ${workspaceFolder}/misc/gdb-macros",
            "miDebuggerServerAddress": "localhost:1234",
            "cwd": "${workspaceFolder}",
            "useExtendedRemote": true,
            "stopAtEntry": true,
            "stopAtConnect": true,
            "setupCommands": [
                {
                    "description": "Enable pretty-printing for gdb",
                    "text": "-enable-pretty-printing",
                    "ignoreFailures": true
                },
                {
                    "description": "Set Disassembly Flavor to Intel",
                    "text": "-gdb-set disassembly-flavor intel",
                    "ignoreFailures": true
                }
            ]
        }
    ]
}
```

### Debugging Workflow

1. **Start PintOS in debug mode** (in a separate terminal):
   ```bash
   cd pintos/src/threads
   ../utils/pintos --qemu --gdb -- run alarm-multiple
   ```
   PintOS will pause and wait for the debugger to connect.

2. **Set breakpoints in VSCode**:
   - Open a source file (e.g., `thread.c`)
   - Click on the line number margin to set a breakpoint
   - For example, click next to the line with `ASSERT(function != NULL)`

3. **Start debugging**:
   - Go to: `Run → Start Debugging`
   - Or press `F5`
   - VSCode will connect to the running PintOS instance and pause at your breakpoints

### Debugging Other Projects

To debug different PintOS projects (e.g., `vm` instead of `threads`):
- Replace `threads` with your project directory name in the `launch.json` configuration
- Update all paths accordingly:
  ```json
  "program": "${workspaceFolder}/vm/build/kernel.o",
  "miDebuggerArgs": "-s ${workspaceFolder}/vm/build/kernel.o -x ${workspaceFolder}/misc/gdb-macros",
  ```

---

## Summary

PintOS can be debugged using three different approaches:

| Tool | Pros | Cons |
|------|------|------|
| **DDD** | Mature GUI debugger, good visualization | Requires separate terminal |
| **Eclipse** | Full IDE with project management | Heavier application, more setup |
| **VSCode** | Lightweight, modern interface, integrates source view with debugging | Minimal compared to Eclipse |

Choose the debugging tool that best fits your workflow and preferences. All three provide equivalent debugging capabilities for PintOS development.

---

## Quick Reference

### Common Commands

```bash
# View PintOS usage
pintos --help

# Run a specific test
pintos --qemu -- run test-name

# Run tests with verbose output
pintos --qemu -v -- run test-name

# Check all tests (in threads/build)
make check

# Clean and rebuild
make clean
make
```

### Important Paths

- **Source code**: `pintos/src/`
- **Threads project**: `pintos/src/threads/`
- **Tests**: `pintos/src/tests/`
- **Utilities**: `pintos/src/utils/`
- **VM project**: `pintos/src/vm/`
- **GDB macros**: `pintos/src/misc/gdb-macros`
- **Build output**: `pintos/src/threads/build/`

---

## Additional Resources

- [Official PintOS Manual](https://www.scs.stanford.edu/23wi-cs212/pintos/pintos_1.html)
- [Debugging Section](https://www.scs.stanford.edu/23wi-cs212/pintos/pintos_10.html#SEC145)
- [Extended Setup Guide](https://docs.google.com/document/d/1YqSes5JwS96W0yLYuSyUg-R9lmdQHzOrCPQW0OHPvZQ/edit?tab=t.0)
