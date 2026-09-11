#pragma once

#include <memory>
#include <string>
#include <vector>

namespace sensors {

struct KeyboardDevice {
    std::string id;
    std::string name;
    std::string path;
    std::string location;
    std::string error;
    bool available = false;
};

struct KeyEvent {
    unsigned code = 0;
    int value = 0;
    std::string name;
    unsigned long long timestamp_ms = 0;
};

struct KeyboardSnapshot {
    std::vector<KeyboardDevice> devices;
    std::string selected_id;
    std::string error;
    bool monitoring = false;
    std::vector<unsigned> pressed;
    std::vector<KeyEvent> events;
    unsigned long long sequence = 0;
};

class KeyboardSource {
public:
    virtual ~KeyboardSource() = default;
    virtual KeyboardSnapshot snapshot() = 0;
    virtual void set_active(bool active) = 0;
    virtual void select_device(const std::string &id) = 0;
    virtual void rescan() = 0;
    virtual void clear_history() = 0;
};

// Platform-independent evdev state reduction also covers lost-event recovery.
class KeyboardEventState {
public:
    explicit KeyboardEventState(KeyboardSnapshot &snapshot) : snapshot_(snapshot) {}
    // True requests EVIOCGKEY after the SYN_REPORT following SYN_DROPPED.
    bool consume(unsigned type, unsigned code, int value, unsigned long long timestamp_ms);
    void resync(std::vector<unsigned> pressed);
    void clear_history();
    void reset();

private:
    KeyboardSnapshot &snapshot_;
    bool dropped_ = false;
};

std::unique_ptr<KeyboardSource> make_keyboard_source();
std::string key_name(unsigned code);

}  // namespace sensors
