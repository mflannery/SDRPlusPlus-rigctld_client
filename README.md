# rigctld_client

An [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) module that forwards every frequency change in SDR++ to a [rigctld](https://hamlib.github.io/Hamlib/rigctld.1.html) daemon, so that a physical radio controlled by Hamlib tracks whatever you tune in SDR++.

This module was created for SDR++ 1.2.1 running on a Fedora 43 Workstation.  It may or may not work on other versions of Fedora or other linux distributions. 

There is compiled shared object file located in the build directory.  This was compiled on Fedora 43 Workstation for SDR++ version 1.2.1.  I have not tested this on any other linux distribution.  If you want to try this, you can copy the .so file from the build folder into the plugins directory.  On my version of Fedora this is located in /usr/lib64/sdrpp/plugins.  You can verify where your plugins folder is by running

```bash
grep modulesDirectory ~/.config/sdrpp/config.json
```

If moving this module int the plugins directory causes SDR++ to crash upon startup, just move the file out of the plugins directory.  

To use the module, first add it from the module manager on the left side of the screen.  Next, add the server where you have rigctld running (localserver or 127.0.0.1) and then add the port rigctld is listening on (this was specified when rigctld was started) and then click "enable" and "connect".  If you see a green "connected" next to status then you are good to go.

![SDR++ version](https://img.shields.io/badge/SDR++-v1.2.1-blue)
![License](https://img.shields.io/badge/license-MIT-green)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)

---

## How it works

When you tune in SDR++ — by clicking the waterfall, clicking within the spectrum, typing a frequency, or using a bookmark — the module computes the absolute dial frequency (hardware centre frequency + VFO offset) and sends it to `rigctld` using the standard Hamlib TCP protocol command `F <hz>`. The `rigctld` daemon then translates that into the appropriate CAT command for your radio.

Two mechanisms ensure the rig always tracks SDR++:

- **`onRetune` event** — fires when the hardware centre frequency changes (dragging the spectrum)
- **Per-frame VFO polling** — detects when you click within the visible spectrum without recentering, catching VFO offset changes that don't trigger a retune event

---

## Requirements

| Requirement | Notes |
|---|---|
| SDR++ v1.2.1 source | Must match your installed SDR++ version exactly |
| CMake ≥ 3.13 | `sudo dnf install cmake` / `sudo apt install cmake` |
| GCC / Clang with C++17 | `sudo dnf install gcc-c++` / `sudo apt install build-essential` |
| Hamlib | `sudo dnf install hamlib` / `sudo apt install libhamlib-dev` |

> **Important:** The module must be compiled against the **exact same source commit** as your installed SDR++ binary. Mismatched builds can cause symbol lookup errors or crashes at startup.
>
> To find your installed version:
> ```bash
> # Fedora/RPM
> rpm -q sdrpp
>
> # Debian/Ubuntu
> dpkg -l sdrpp
> ```

---

## Building

The correct approach is to build the module **inside the SDR++ source tree**, which guarantees symbol compatibility with your installed binary.

### 1. Clone the matching SDR++ source

Find the git commit hash from your installed package and check it out:

```bash
# Example for Fedora package version 1.2.1-20251212.0.git65a0e11d
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
cd SDRPlusPlus
git checkout 65a0e11d
```

### 2. Place the module in the source tree

```bash
cp -r rigctld_client ~/SDRPlusPlus/misc_modules/rigctld_client
```

Your directory structure should look like this:

```
SDRPlusPlus/
└── misc_modules/
    ├── frequency_manager/
    ├── recorder/
    ├── rigctld_client/       ← this module
    │   ├── CMakeLists.txt
    │   └── src/
    │       └── main.cpp
    └── ...
```

### 3. Register the module in SDR++'s root CMakeLists.txt

Open `~/SDRPlusPlus/CMakeLists.txt` and find the block where other `misc_modules` are listed. Each module follows this pattern:

```cmake
if (OPT_BUILD_RECORDER)
    add_subdirectory("misc_modules/recorder")
endif (OPT_BUILD_RECORDER)
```

Add the following block alongside them:

```cmake
if (OPT_BUILD_RIGCTLD_CLIENT)
    add_subdirectory("misc_modules/rigctld_client")
endif (OPT_BUILD_RIGCTLD_CLIENT)
```

### 4. Configure and build

```bash
cd ~/SDRPlusPlus
mkdir -p build && cd build

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DOPT_BUILD_RIGCTLD_CLIENT=ON

make rigctld_client -j$(nproc)
```

The output is `misc_modules/rigctld_client/rigctld_client.so`.

---

## Installation

Copy the `.so` to your SDR++ plugins directory. On Fedora this is typically `/usr/lib64/sdrpp/plugins/`:

```bash
sudo cp ~/SDRPlusPlus/build/misc_modules/rigctld_client/rigctld_client.so \
    /usr/lib64/sdrpp/plugins/
```

To find the correct plugins directory on your system:

```bash
find /usr -name "audio_sink.so" 2>/dev/null
```

SDR++ automatically scans the plugins directory on startup — **do not** add the path manually to `config.json`, as this will cause it to be loaded twice and fail.

---

## Usage

### 1. Start rigctld

Start `rigctld` connected to your radio before launching SDR++:

```bash
rigctld -m <model> -r /dev/ttyUSB0 -s 9600 -t 4532
```

Replace `-m <model>` with the Hamlib model number for your radio. Find it with:

```bash
rigctl --list | grep -i <your_radio_name>
```

Common examples:

| Radio | Model number |
|---|---|
| Yaesu FT-100D | 1012 |
| Yaesu FT-991A | 1044 |
| Icom IC-7300 | 3073 |
| Kenwood TS-2000 | 222 |

### 2. Add the module in SDR++

1. Start SDR++
2. Open **Menu → Module Manager**
3. Type a name (e.g. `My Rig`) in the name field
4. Select `rigctld_client` from the module dropdown
5. Click **+** to create the instance

A new panel named `My Rig` will appear in the left sidebar.

### 3. Connect and enable

In the `rigctld_client` panel:

1. Enter the **Host** where `rigctld` is running (default: `localhost`)
2. Enter the **Port** (default: `4532`)
3. Click **Connect**
4. Click **Enable**

Tune in SDR++ — the rig will follow.

---

## Panel reference

| Control | Description |
|---|---|
| **Enable / Enabled** | Arms frequency forwarding. Greyed out when active. |
| **Host** | Hostname or IP address of the machine running `rigctld`. |
| **Port** | TCP port `rigctld` listens on (default `4532`). |
| **Connect** | Opens the TCP connection to `rigctld`. |
| **Disconnect** | Closes the connection and disables the module. |
| **Send Current Freq** | Manually pushes the current dial frequency to the rig once. Useful after connecting if the rig is out of sync. |
| **Status** | Green = connected, red = not connected. Shows last error message. |
| **Last sent** | The most recently transmitted frequency in MHz. |

---

## Configuration

Settings are saved automatically to:

```
~/.config/sdrpp/rigctld_client_config.json
```

Example:

```json
{
    "My Rig": {
        "host": "localhost",
        "port": 4532,
        "enabled": true
    }
}
```

---

## Protocol reference

The module communicates with `rigctld` using the Hamlib extended TCP protocol:

```
F <frequency_hz>\n      →  set VFO frequency
RPRT 0\n                ←  success response (read and discarded)
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Module doesn't appear in Module Manager dropdown | `.so` not in plugins directory, or wrong plugins directory | Check `sdrpp 2>&1 \| grep -i "rigctld\|load"` to see where SDR++ is looking |
| `undefined symbol` error on startup | Module built against wrong SDR++ version | Rebuild against the exact commit matching your installed binary |
| Module loads twice / "same name" error | Path added to both plugins directory and `config.json` | Remove the manual entry from `config.json` |
| "Connection failed" | `rigctld` not running, wrong host/port, or firewall | Verify with `rigctl -m <model> -r /dev/ttyUSB0 f` |
| Rig doesn't move | PTT/CAT not working at the `rigctld` level | Test independently with `rigctl -m <model> -r /dev/ttyUSB0 F 14200000` |
| SDR++ crashes on startup | ABI mismatch between module and core | Rebuild — see Building section above |

---

## License

MIT — do whatever you like with it.
