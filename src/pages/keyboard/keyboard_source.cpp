#include "pages/keyboard/keyboard_source.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <utility>

#ifdef __linux__
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace sensors {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr unsigned key_type = 1;
constexpr unsigned sync_type = 0;
constexpr unsigned sync_report = 0;
constexpr unsigned sync_dropped = 3;
constexpr unsigned max_key_code = 0x2ff;
constexpr std::size_t history_limit = 100;

std::string read_name(const fs::path &path)
{
    std::ifstream input(path);
    std::string text;
    std::getline(input, text);
    if (text.size() > 512) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return {};
    const auto start = text.find_first_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

fs::path root_path(const char *variable, const char *fallback)
{
    const auto *value = std::getenv(variable);
    return value && *value ? value : fallback;
}

fs::path canonical_path(const fs::path &path)
{
    std::error_code error;
    const auto resolved = fs::weakly_canonical(path, error);
    return error ? path : resolved;
}

std::string location(const fs::path &path)
{
    static const std::regex client("([0-9]+)-[0-9a-fA-F]{4}");
    for (const auto &part : canonical_path(path)) {
        std::smatch match;
        const auto text = part.string();
        if (std::regex_match(text, match, client)) return "i2c-" + match[1].str();
    }
    return "input";
}

bool same_devices(const std::vector<KeyboardDevice> &a, const std::vector<KeyboardDevice> &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].path != b[i].path ||
            a[i].location != b[i].location || a[i].error != b[i].error ||
            a[i].available != b[i].available) return false;
    }
    return true;
}

class EvdevKeyboardSource final : public KeyboardSource {
public:
    EvdevKeyboardSource()
        : sys_(root_path("LILYGO_SENSORS_SYS_ROOT", "/sys")),
          dev_(root_path("LILYGO_SENSORS_DEV_ROOT", "/dev")), events_(state_)
    {
        discover();
    }

    ~EvdevKeyboardSource() override { close_device(); }

    KeyboardSnapshot snapshot() override
    {
        if (Clock::now() >= next_scan_) discover();
        if (active_) {
            open_device();
            read_events();
        }
        return state_;
    }

    void set_active(bool active) override
    {
        if (active_ == active) return;
        active_ = active;
        ++state_.sequence;
        if (!active_) {
            close_device();
            events_.resync({});
        } else {
            discover();
            open_device();
        }
    }

    void select_device(const std::string &id) override
    {
        if (id == state_.selected_id) return;
        const auto found = std::find_if(state_.devices.begin(), state_.devices.end(),
                                       [&](const auto &device) { return device.id == id; });
        if (found == state_.devices.end()) return;
        close_device();
        events_.reset();
        state_.selected_id = id;
        state_.error = found->error;
        retry_after_ = {};
        ++state_.sequence;
        if (active_) open_device();
    }

    void rescan() override
    {
        retry_after_ = {};
        discover();
        if (active_) open_device();
    }

    void clear_history() override { events_.clear_history(); }

private:
    void discover()
    {
        next_scan_ = Clock::now() + std::chrono::seconds(2);
        std::vector<KeyboardDevice> devices;
        std::error_code error;
        fs::directory_iterator it(sys_ / "class/input", error), end;
        while (!error && it != end) {
            const auto entry = it->path();
            const auto filename = entry.filename().string();
            const bool event_node = filename.size() > 5 && filename.compare(0, 5, "event") == 0 &&
                std::all_of(filename.begin() + 5, filename.end(), [](unsigned char c) {
                    return std::isdigit(c);
                });
            if (event_node) {
                const auto name = read_name(entry / "device/name");
                auto folded = name;
                std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (folded.find("tca8418") != std::string::npos) {
                    KeyboardDevice device;
                    device.id = canonical_path(entry).string();
                    device.name = name;
                    device.path = (dev_ / "input" / filename).string();
                    device.location = location(entry / "device");
#ifdef __linux__
                    device.available = ::access(device.path.c_str(), R_OK) == 0;
                    if (!device.available) device.error = std::strerror(errno);
#else
                    device.error = "Keyboard events require Linux evdev";
#endif
                    devices.push_back(std::move(device));
                }
            }
            it.increment(error);
        }
        std::sort(devices.begin(), devices.end(), [](const auto &a, const auto &b) {
            return a.path < b.path;
        });
        if (!same_devices(state_.devices, devices)) {
            state_.devices = std::move(devices);
            ++state_.sequence;
        }
        const auto selected = std::find_if(state_.devices.begin(), state_.devices.end(),
            [&](const auto &device) { return device.id == state_.selected_id; });
        if (selected == state_.devices.end()) {
            close_device();
            events_.reset();
            const auto id = state_.devices.empty() ? std::string{} : state_.devices.front().id;
            if (state_.selected_id != id) {
                state_.selected_id = id;
                retry_after_ = {};
                ++state_.sequence;
            }
        }
        if (state_.devices.empty()) {
            set_error(error && error != std::errc::no_such_file_or_directory
                          ? "Input discovery: " + error.message()
                          : "TCA8418 keyboard not found");
        } else if (!state_.monitoring) {
            const auto *device = selected_device();
            if (device && !device->error.empty()) set_error(device->error);
            else if (Clock::now() >= retry_after_) set_error({});
        }
    }

    const KeyboardDevice *selected_device() const
    {
        const auto found = std::find_if(state_.devices.begin(), state_.devices.end(),
            [&](const auto &device) { return device.id == state_.selected_id; });
        return found == state_.devices.end() ? nullptr : &*found;
    }

    void set_error(std::string error)
    {
        if (state_.error == error) return;
        state_.error = std::move(error);
        ++state_.sequence;
    }

    void close_device()
    {
#ifdef __linux__
        if (fd_ >= 0) ::close(fd_);
#endif
        fd_ = -1;
        if (state_.monitoring) {
            state_.monitoring = false;
            ++state_.sequence;
        }
    }

    void fail(const std::string &operation)
    {
        const auto error = operation + ": " + std::strerror(errno);
        close_device();
        events_.resync({});
        set_error(error);
        retry_after_ = Clock::now() + std::chrono::seconds(2);
    }

    bool resync_pressed()
    {
#ifdef __linux__
        std::array<unsigned char, (KEY_MAX + 8) / 8> bits{};
        if (::ioctl(fd_, EVIOCGKEY(bits.size()), bits.data()) < 0) {
            fail("Read keyboard state");
            return false;
        }
        std::vector<unsigned> pressed;
        for (unsigned code = 1; code <= KEY_MAX; ++code) {
            if ((bits[code / 8] >> (code % 8)) & 1) pressed.push_back(code);
        }
        events_.resync(std::move(pressed));
        return true;
#else
        return false;
#endif
    }

    void open_device()
    {
        if (fd_ >= 0 || !active_ || Clock::now() < retry_after_) return;
        const auto *device = selected_device();
        if (!device) return;
#ifdef __linux__
        fd_ = ::open(device->path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            fail("Open " + device->path);
            return;
        }
        std::array<char, 512> name{};
        if (::ioctl(fd_, EVIOCGNAME(name.size()), name.data()) < 0) {
            fail("Identify keyboard");
            return;
        }
        std::string folded(name.data(), ::strnlen(name.data(), name.size()));
        std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (folded.find("tca8418") == std::string::npos) {
            close_device();
            set_error("Selected event device is no longer TCA8418");
            retry_after_ = Clock::now() + std::chrono::seconds(2);
            return;
        }
        int event_clock = CLOCK_MONOTONIC;
        monotonic_events_ = ::ioctl(fd_, EVIOCSCLOCKID, &event_clock) == 0;
        if (!resync_pressed()) return;
        set_error({});
        state_.monitoring = true;
        ++state_.sequence;
#else
        set_error("Keyboard events require Linux evdev");
#endif
    }

    void read_events()
    {
#ifdef __linux__
        if (fd_ < 0) return;
        // Bound each UI poll even if a device continuously produces events.
        for (unsigned batch = 0; batch < 8; ++batch) {
            pollfd poll_fd{fd_, POLLIN, 0};
            const int ready = ::poll(&poll_fd, 1, 0);
            if (ready < 0 && errno == EINTR) return;
            if (ready < 0) { fail("Poll keyboard"); return; }
            if (!ready) return;
            if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                errno = ENODEV;
                fail("Keyboard disconnected");
                return;
            }
            if (!(poll_fd.revents & POLLIN)) return;
            std::array<input_event, 32> buffer{};
            const auto bytes = ::read(fd_, buffer.data(), sizeof(buffer));
            if (bytes < 0 && (errno == EAGAIN || errno == EINTR)) return;
            if (bytes <= 0) {
                if (!bytes) errno = ENODEV;
                fail("Read keyboard events");
                return;
            }
            if (static_cast<std::size_t>(bytes) % sizeof(input_event)) {
                errno = EIO;
                fail("Incomplete keyboard event");
                return;
            }
            const auto count = static_cast<std::size_t>(bytes) / sizeof(input_event);
            for (std::size_t i = 0; i < count; ++i) {
                const auto &event = buffer[i];
                const auto timestamp = monotonic_events_
                    ? static_cast<unsigned long long>(event.time.tv_sec) * 1000 +
                      static_cast<unsigned long long>(event.time.tv_usec) / 1000
                    : static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                          Clock::now().time_since_epoch()).count());
                if (events_.consume(event.type, event.code, event.value, timestamp)) {
                    if (!resync_pressed()) return;
                }
            }
        }
#endif
    }

    fs::path sys_;
    fs::path dev_;
    KeyboardSnapshot state_;
    KeyboardEventState events_;
    bool active_ = false;
#ifdef __linux__
    bool monotonic_events_ = false;
#endif
    int fd_ = -1;
    Clock::time_point next_scan_{};
    Clock::time_point retry_after_{};
};

}  // namespace

std::string key_name(unsigned code)
{
    // Linux input-event codes are stable across architectures and simulators.
    if (code >= 2 && code <= 10) return std::to_string(code - 1);
    if (code >= 16 && code <= 25) return std::string(1, "QWERTYUIOP"[code - 16]);
    if (code >= 30 && code <= 38) return std::string(1, "ASDFGHJKL"[code - 30]);
    if (code >= 44 && code <= 50) return std::string(1, "ZXCVBNM"[code - 44]);
    if (code >= 59 && code <= 68) return "F" + std::to_string(code - 58);
    switch (code) {
    case 1: return "Esc";
    case 11: return "0";
    case 12: return "Minus";
    case 13: return "Equal";
    case 14: return "Backspace";
    case 15: return "Tab";
    case 26: return "Left bracket";
    case 27: return "Right bracket";
    case 28: return "Enter";
    case 29: return "Left Ctrl";
    case 39: return "Semicolon";
    case 40: return "Apostrophe";
    case 41: return "Grave";
    case 42: return "Left Shift";
    case 43: return "Backslash";
    case 51: return "Comma";
    case 52: return "Period";
    case 53: return "Slash";
    case 54: return "Right Shift";
    case 56: return "Left Alt";
    case 57: return "Space";
    case 58: return "Caps Lock";
    case 87: return "F11";
    case 88: return "F12";
    case 97: return "Right Ctrl";
    case 100: return "Right Alt";
    case 103: return "Up";
    case 105: return "Left";
    case 106: return "Right";
    case 108: return "Down";
    case 111: return "Delete";
    case 116: return "Power";
    case 125: return "Left Meta";
    case 126: return "Right Meta";
    case 464: return "Fn";
    case 582: return "Voice command";
    default: return "KEY_" + std::to_string(code);
    }
}

bool KeyboardEventState::consume(unsigned type, unsigned code, int value,
                                 unsigned long long timestamp_ms)
{
    if (type == sync_type && code == sync_dropped) {
        dropped_ = true;
        if (!snapshot_.pressed.empty()) {
            snapshot_.pressed.clear();
            ++snapshot_.sequence;
        }
        return false;
    }
    if (dropped_) return type == sync_type && code == sync_report;
    if (type != key_type || code == 0 || code > max_key_code || value < 0 || value > 2) return false;
    auto &pressed = snapshot_.pressed;
    const auto match = std::lower_bound(pressed.begin(), pressed.end(), code);
    if (value == 0) {
        if (match != pressed.end() && *match == code) pressed.erase(match);
    } else if (match == pressed.end() || *match != code) {
        pressed.insert(match, code);
    }
    snapshot_.events.push_back({code, value, key_name(code), timestamp_ms});
    if (snapshot_.events.size() > history_limit) snapshot_.events.erase(snapshot_.events.begin());
    ++snapshot_.sequence;
    return false;
}

void KeyboardEventState::resync(std::vector<unsigned> pressed)
{
    pressed.erase(std::remove_if(pressed.begin(), pressed.end(), [](unsigned code) {
        return code == 0 || code > max_key_code;
    }), pressed.end());
    std::sort(pressed.begin(), pressed.end());
    pressed.erase(std::unique(pressed.begin(), pressed.end()), pressed.end());
    if (snapshot_.pressed != pressed) {
        snapshot_.pressed = std::move(pressed);
        ++snapshot_.sequence;
    }
    dropped_ = false;
}

void KeyboardEventState::clear_history()
{
    if (snapshot_.events.empty()) return;
    snapshot_.events.clear();
    ++snapshot_.sequence;
}

void KeyboardEventState::reset()
{
    resync({});
    clear_history();
}

std::unique_ptr<KeyboardSource> make_keyboard_source()
{
    return std::make_unique<EvdevKeyboardSource>();
}

}  // namespace sensors
