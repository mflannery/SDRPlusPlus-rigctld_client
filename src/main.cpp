/**
 * rigctld_client — SDR++ module
 *
 * Connects SDR++ to a running rigctld daemon so that every VFO frequency
 * change inside SDR++ is forwarded to the rig via the rigctld TCP protocol.
 *
 * Protocol used:
 *   F <freq_hz>\n   — set VFO frequency
 *
 * The module registers a retune event handler on sigpath::sourceManager so
 * that it is notified whenever SDR++ changes the centre frequency (i.e. the
 * hardware frequency), and it also polls the currently selected VFO offset so
 * the absolute dial frequency is sent to the rig.
 *
 * Build with the CMakeLists.txt in the parent directory.
 */

#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <signal_path/vfo_manager.h>
#include <core.h>
#include <config.h>
#include <utils/optionlist.h>

#include <cstring>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

// POSIX networking
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// ---------------------------------------------------------------------------
// Module metadata
// ---------------------------------------------------------------------------
SDRPP_MOD_INFO{
    /* Name:        */ "rigctld_client",
    /* Description: */ "Forwards SDR++ VFO frequency to a rigctld daemon",
    /* Author:      */ "SDR++ Community",
    /* Version:     */ 0, 2, 0,
    /* Max instances*/ -1
};

// ---------------------------------------------------------------------------
// Persistent configuration keys
// ---------------------------------------------------------------------------
ConfigManager config;

// ---------------------------------------------------------------------------
// Helper – open a blocking TCP connection, returns fd or -1
// ---------------------------------------------------------------------------
static int tcp_connect(const char* host, int port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

// ---------------------------------------------------------------------------
// Send a frequency to rigctld.  Returns true on success.
// The rigctld "set_freq" extended command is:  F <hz>\n
// ---------------------------------------------------------------------------
static bool send_freq(int fd, long long freq_hz) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "F %lld\n", freq_hz);
    ssize_t sent = 0;
    while (sent < n) {
        ssize_t r = write(fd, buf + sent, n - sent);
        if (r <= 0) return false;
        sent += r;
    }
    // Read (and discard) the RPRT response line so the socket stays in sync
    char resp[64];
    ssize_t nr = read(fd, resp, sizeof(resp) - 1);
    (void)nr;
    return true;
}

// ---------------------------------------------------------------------------
// Module class
// ---------------------------------------------------------------------------
class RigctldClientModule : public ModuleManager::Instance {
public:
    RigctldClientModule(std::string name) : _name(name) {
        // Load or create default config
        config.acquire();
        if (!config.conf.contains(_name)) {
            config.conf[_name]["host"]          = "localhost";
            config.conf[_name]["port"]          = 4532;
            config.conf[_name]["enabled"]       = false;
            config.conf[_name]["poll_interval"] = 500;
        }
        strncpy(_host, config.conf[_name]["host"].get<std::string>().c_str(),
                sizeof(_host) - 1);
        _port          = config.conf[_name]["port"];
        _enabled       = config.conf[_name]["enabled"];
        _poll_interval = config.conf[_name]["poll_interval"];
        config.release(true);

        // Register retune event – fired when the *hardware* centre freq changes
        _retuneHandler.ctx     = this;
        _retuneHandler.handler = _onRetune;
        sigpath::sourceManager.onRetune.bindHandler(&_retuneHandler);

        // Register the side-menu panel
        gui::menu.registerEntry(_name, _menuHandler, this, NULL);

        if (_enabled) _connect();
    }

    ~RigctldClientModule() {
        _disconnect();
        sigpath::sourceManager.onRetune.unbindHandler(&_retuneHandler);
        gui::menu.removeEntry(_name);
    }

    void postInit() {}


    void enable()  { _enabled = true;  _saveConfig(); _connect();    }
    void disable() { _enabled = false; _saveConfig(); _disconnect(); }
    bool isEnabled() { return _enabled; }

private:
    // -----------------------------------------------------------------------
    // Connection management
    // -----------------------------------------------------------------------
    void _connect() {
        std::lock_guard<std::mutex> lk(_sockMtx);
        if (_sockFd >= 0) return;            // already connected
        _sockFd = tcp_connect(_host, _port);
        if (_sockFd >= 0) {
            _statusMsg  = "Connected to " + std::string(_host) + ":" +
                          std::to_string(_port);
            _connected  = true;
        } else {
            _statusMsg  = "Connection failed";
            _connected  = false;
        }
    }

    void _disconnect() {
        std::lock_guard<std::mutex> lk(_sockMtx);
        if (_sockFd >= 0) {
            close(_sockFd);
            _sockFd    = -1;
        }
        _connected = false;
        _statusMsg = "Disconnected";
    }

    // -----------------------------------------------------------------------
    // Send frequency (reconnects on failure)
    // -----------------------------------------------------------------------
    void _sendFreq(long long hz) {
        std::lock_guard<std::mutex> lk(_sockMtx);
        if (!_enabled) return;
        if (_sockFd < 0) {
            // Try to reconnect silently
            _sockFd = tcp_connect(_host, _port);
            if (_sockFd < 0) {
                _connected = false;
                _statusMsg = "Not connected – retrying…";
                return;
            }
            _connected = true;
            _statusMsg = "Re-connected to " + std::string(_host) + ":" +
                         std::to_string(_port);
        }
        if (!send_freq(_sockFd, hz)) {
            close(_sockFd);
            _sockFd    = -1;
            _connected = false;
            _statusMsg = "Send error – will retry";
        } else {
            _lastSentFreq = hz;
        }
    }

    // -----------------------------------------------------------------------
    // Compute absolute dial frequency from centre + VFO offset.
    // centreHz is the hardware centre frequency delivered by the retune event.
    // -----------------------------------------------------------------------
    long long _dialFreq(long long centreHz) {
        // Add the currently selected VFO offset so the rig tracks the actual
        // displayed dial frequency rather than the hardware centre frequency.
        // getOffset() may throw if the VFO doesn't exist yet, so guard it.
        try {
            double offset = sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
            centreHz += (long long)offset;
        }
        catch (...) {
            // No VFO selected or not yet created – send centre freq as-is.
        }
        return centreHz;
    }

    // -----------------------------------------------------------------------
    // Poll current dial frequency every frame and send if it has changed.
    // This catches VFO clicks within the spectrum that don't trigger onRetune.
    // -----------------------------------------------------------------------
    void _pollFreq() {
        if (!_enabled || !_connected) return;
        long long hz = _dialFreq(_lastCentreFreq);
        if (hz != _lastSentFreq && hz > 0) {
            _sendFreq(hz);
        }
    }

    // -----------------------------------------------------------------------
    // Static event handler – called by SDR++ on every hardware retune.
    // The 'freq' argument IS the new centre frequency – no need to query it.
    // -----------------------------------------------------------------------
    static void _onRetune(double freq, void* ctx) {
        auto* self = static_cast<RigctldClientModule*>(ctx);
        if (!self->_enabled) return;
        self->_lastCentreFreq = (long long)freq;
        long long hz = self->_dialFreq((long long)freq);
        self->_sendFreq(hz);
    }

    // -----------------------------------------------------------------------
    // Config persistence
    // -----------------------------------------------------------------------
    void _saveConfig() {
        config.acquire();
        config.conf[_name]["host"]          = std::string(_host);
        config.conf[_name]["port"]          = _port;
        config.conf[_name]["enabled"]       = _enabled;
        config.conf[_name]["poll_interval"] = _poll_interval;
        config.release(true);
    }

    // -----------------------------------------------------------------------
    // ImGui side-panel
    // -----------------------------------------------------------------------
    static void _menuHandler(void* ctx) {
        auto* self = static_cast<RigctldClientModule*>(ctx);
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // Poll every frame to catch VFO offset changes (clicks in the spectrum)
        self->_pollFreq();

        // Enable / disable toggle
        if (!self->_enabled) {
            if (ImGui::Button("Enable##rigctld", ImVec2(menuWidth, 0))) {
                self->enable();
            }
        } else {
            style::beginDisabled();
            ImGui::Button("Enabled##rigctld", ImVec2(menuWidth, 0));
            style::endDisabled();
        }

        ImGui::Separator();

        // Host
        ImGui::Text("rigctld Host");
        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::InputText("##rigctld_host", self->_host,
                             sizeof(self->_host),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (self->_enabled) { self->_disconnect(); self->_connect(); }
            self->_saveConfig();
        }

        // Port
        ImGui::Text("Port");
        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::InputInt("##rigctld_port", &self->_port, 1, 100)) {
            self->_port = std::max(1, std::min(65535, self->_port));
            if (self->_enabled) { self->_disconnect(); self->_connect(); }
            self->_saveConfig();
        }

        ImGui::Separator();

        // Connect / Disconnect buttons
        if (!self->_connected) {
            if (ImGui::Button("Connect##rigctld", ImVec2(menuWidth, 0))) {
                self->_connect();
            }
        } else {
            if (ImGui::Button("Disconnect##rigctld", ImVec2(menuWidth, 0))) {
                self->_disconnect();
                self->_enabled = false;
                self->_saveConfig();
            }
        }

        // Send current frequency manually
        if (self->_connected) {
            ImGui::Spacing();
            if (ImGui::Button("Send Current Freq##rigctld",
                              ImVec2(menuWidth, 0))) {
                self->_sendFreq(self->_dialFreq(self->_lastCentreFreq));
            }
        }

        ImGui::Separator();

        // Status
        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (self->_connected)
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected");
        else
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Not connected");

        ImGui::TextWrapped("%s", self->_statusMsg.c_str());

        if (self->_lastSentFreq > 0) {
            double mhz = self->_lastSentFreq / 1e6;
            ImGui::Text("Last sent: %.6f MHz", mhz);
        }
    }

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------
    std::string _name;

    char  _host[256]  = "localhost";
    int   _port       = 4532;
    bool  _enabled    = false;
    int   _poll_interval = 500;   // ms (reserved for future polling mode)

    std::mutex  _sockMtx;
    int         _sockFd      = -1;
    bool        _connected   = false;
    std::string _statusMsg   = "Disconnected";
    long long   _lastCentreFreq = 0;
    long long   _lastSentFreq = 0;

    EventHandler<double> _retuneHandler;
};

// ---------------------------------------------------------------------------
// Module entry points required by SDR++
// ---------------------------------------------------------------------------
MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/rigctld_client_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RigctldClientModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete static_cast<RigctldClientModule*>(inst);
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
