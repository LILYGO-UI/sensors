#include "pages/keyboard/keyboard_source.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace sensors;

void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Environment {
    struct Value { std::string key, value; bool present; };
    std::vector<Value> previous;
    void set(const std::string &key, const std::string &value)
    {
        const auto *old = std::getenv(key.c_str());
        previous.push_back({key, old ? old : "", old != nullptr});
        ::setenv(key.c_str(), value.c_str(), 1);
    }
    ~Environment()
    {
        for (auto it = previous.rbegin(); it != previous.rend(); ++it) {
            if (it->present) ::setenv(it->key.c_str(), it->value.c_str(), 1);
            else ::unsetenv(it->key.c_str());
        }
    }
};

struct Fixture {
    fs::path root;
    Environment environment;

    Fixture()
    {
        std::string pattern = (fs::temp_directory_path() / "lilygo-keyboard-XXXXXX").string();
        const auto *created = ::mkdtemp(pattern.data());
        require(created != nullptr, "create keyboard fixture");
        root = created;
        fs::create_directories(root / "sys/class/input");
        fs::create_directories(root / "dev/input");
        environment.set("LILYGO_SENSORS_SYS_ROOT", (root / "sys").string());
        environment.set("LILYGO_SENSORS_DEV_ROOT", (root / "dev").string());
    }

    ~Fixture() { std::error_code error; fs::remove_all(root, error); }

    void add(int index, const std::string &name, bool with_device = true)
    {
        const auto input = root / "sys/devices/platform/i2c-1/1-0034/input" /
                           ("input" + std::to_string(index));
        const auto event = "event" + std::to_string(index);
        fs::create_directories(input / event);
        std::ofstream(input / "name") << name << '\n';
        fs::create_directory_symlink(input, input / event / "device");
        fs::create_directory_symlink(input / event, root / "sys/class/input" / event);
        if (with_device) std::ofstream(root / "dev/input" / event) << "";
    }
};

void key_names()
{
    require(key_name(1) == "Esc" && key_name(2) == "1" && key_name(11) == "0", "number and escape keys");
    require(key_name(16) == "Q" && key_name(25) == "P" && key_name(30) == "A" &&
            key_name(38) == "L" && key_name(44) == "Z" && key_name(50) == "M", "letter rows");
    require(key_name(42) == "Left Shift" && key_name(57) == "Space" &&
            key_name(125) == "Left Meta", "board modifiers");
    require(key_name(59) == "F1" && key_name(68) == "F10" && key_name(87) == "F11", "function keys");
    require(key_name(464) == "Fn" && key_name(582) == "Voice command", "board special keys");
    require(key_name(700) == "KEY_700", "unknown keys retain Linux code");
}

void press_release_and_repeat()
{
    KeyboardSnapshot snapshot;
    KeyboardEventState events(snapshot);
    events.consume(4, 4, 30, 10);
    events.consume(1, 0, 1, 10);
    events.consume(1, 900, 1, 10);
    events.consume(1, 30, 3, 10);
    require(snapshot.events.empty() && snapshot.sequence == 0, "ignore scan codes and invalid keys");
    events.consume(1, 42, 1, 20);
    events.consume(1, 30, 1, 21);
    events.consume(1, 30, 2, 22);
    require(snapshot.pressed == std::vector<unsigned>({30, 42}), "chord is sorted and repeat is not duplicated");
    require(snapshot.events.size() == 3 && snapshot.events.back().value == 2 &&
            snapshot.events.back().name == "A" && snapshot.events.back().timestamp_ms == 22,
            "repeat has code, readable name, value, and source timestamp");
    events.consume(1, 30, 0, 23);
    require(snapshot.pressed == std::vector<unsigned>({42}), "release preserves other held keys");
    events.consume(1, 42, 0, 24);
    require(snapshot.pressed.empty() && snapshot.events.size() == 5, "all releases retained");
    require(snapshot.events.front().timestamp_ms < snapshot.events.back().timestamp_ms, "chronological history");
}

void history_and_clear()
{
    KeyboardSnapshot snapshot;
    KeyboardEventState events(snapshot);
    for (unsigned i = 0; i < 140; ++i) events.consume(1, 30, i ? 2 : 1, i);
    require(snapshot.events.size() == 100 && snapshot.events.front().timestamp_ms == 40 &&
            snapshot.events.back().timestamp_ms == 139, "history keeps latest 100 events");
    const auto sequence = snapshot.sequence;
    events.clear_history();
    require(snapshot.events.empty() && snapshot.pressed == std::vector<unsigned>({30}) &&
            snapshot.sequence > sequence, "clear history preserves held state and changes sequence");
    const auto cleared = snapshot.sequence;
    events.clear_history();
    require(snapshot.sequence == cleared, "repeated clear has no spurious update");
    events.consume(1, 30, 0, 141);
    require(snapshot.pressed.empty() && snapshot.events.size() == 1, "release remains correct after clear");
}

void dropped_events_and_resync()
{
    KeyboardSnapshot snapshot;
    KeyboardEventState events(snapshot);
    events.consume(1, 30, 1, 1);
    require(!events.consume(0, 3, 0, 2), "dropped event waits for report before resync");
    require(snapshot.pressed.empty(), "lost events invalidate held state");
    events.consume(1, 30, 0, 3);
    events.consume(1, 42, 1, 4);
    require(snapshot.events.size() == 1 && snapshot.pressed.empty(), "ignore events through dropped packet");
    require(events.consume(0, 0, 0, 5), "SYN_REPORT requests kernel key-state query");
    events.resync({42, 30, 42, 0, 900});
    require(snapshot.pressed == std::vector<unsigned>({30, 42}), "kernel resync replaces state and normalizes keys");
    require(snapshot.events.size() == 1, "resync does not invent key events");
    events.consume(1, 42, 0, 6);
    require(snapshot.pressed == std::vector<unsigned>({30}) && snapshot.events.size() == 2,
            "normal event processing resumes after resync");
    require(!events.consume(0, 0, 0, 7), "normal reports do not trigger extra ioctl");
    events.reset();
    require(snapshot.pressed.empty() && snapshot.events.empty(), "device change discards previous device state");
}

void discovery_and_selection()
{
    Fixture fixture;
    fixture.add(1, "USB Keyboard");
    fixture.add(3, "  TCA8418 Keypad  ");
    fixture.add(8, "tca8418");
    auto source = make_keyboard_source();
    auto snapshot = source->snapshot();
    require(snapshot.devices.size() == 2, "only TCA8418 devices are discovered");
    require(snapshot.devices[0].name == "TCA8418 Keypad", "sysfs names are trimmed");
    require(snapshot.devices[0].location == "i2c-1", "input symlink resolves I2C location");
    require(snapshot.devices[0].path == (fixture.root / "dev/input/event3").string(), "device root override");
    require(snapshot.selected_id == snapshot.devices[0].id && !snapshot.monitoring,
            "first board keyboard selected without opening until page active");
    const auto first = snapshot.selected_id;
    const auto second = snapshot.devices[1].id;
    source->select_device("not-a-keyboard");
    require(source->snapshot().selected_id == first, "cannot select a non-TCA8418 device");
    source->select_device(second);
    require(source->snapshot().selected_id == second, "select another board keyboard");
    source->set_active(true);
    snapshot = source->snapshot();
    require(!snapshot.monitoring && !snapshot.error.empty() && snapshot.events.empty(),
            "non-evdev files or unsupported platforms report error without reading arbitrary files");
    source->set_active(false);
    require(!source->snapshot().monitoring && source->snapshot().pressed.empty(), "inactive source has no held keys");
    fs::remove(fixture.root / "sys/class/input/event8");
    source->rescan();
    snapshot = source->snapshot();
    require(snapshot.devices.size() == 1 && snapshot.selected_id == first, "removed selection falls back to remaining board keyboard");
    fs::remove(fixture.root / "sys/class/input/event3");
    source->rescan();
    snapshot = source->snapshot();
    require(snapshot.devices.empty() && snapshot.selected_id.empty() && !snapshot.error.empty(),
            "unplugged keyboard has explicit empty state");
    fixture.add(4, "tca8418", false);
    source->rescan();
    snapshot = source->snapshot();
    require(snapshot.devices.size() == 1 && !snapshot.devices[0].available &&
            !snapshot.devices[0].error.empty(), "missing event node retains discovered device and error");
}

}  // namespace

int main()
{
    try {
        key_names();
        press_release_and_repeat();
        history_and_clear();
        dropped_events_and_resync();
        discovery_and_selection();
        std::cout << "keyboard source tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
