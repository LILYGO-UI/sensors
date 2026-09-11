#include "pages/sensors/sensors_view_model.hpp"

#include <cassert>
#include <iostream>
#include <unistd.h>

using namespace sensors;

namespace {
class FixtureSource final : public SensorSource {
public:
    Snapshot data;
    unsigned interval = 0;
    unsigned rescans = 0;
    Snapshot snapshot() override { return data; }
    void rescan() override { ++rescans; }
    void set_interval(unsigned value) override { interval = value; }
};
}  // namespace

int main()
{
    lv_init();
    {
        auto source = std::make_unique<FixtureSource>();
        auto *fixture = source.get();
        fixture->data.sequence = 1;
        fixture->data.timestamp = Clock::now();
        fixture->data.sensors = {
            {"accel", "Accelerometer", "accelerometer", "qmi8658c", "i2c-1", "fixture", "", true,
             {{"X", "m/s2", 1}, {"Y", "m/s2", 0}, {"Z", "m/s2", 9.8}}, Clock::now(), "imu", {}},
            {"gyro", "Gyroscope", "gyroscope", "qmi8658c", "i2c-1", "fixture", "", true,
             {{"X", "rad/s", 0.5}, {"Y", "rad/s", 0}, {"Z", "rad/s", 0.1}}, Clock::now(), "imu", {}}};
        SensorsViewModel vm(std::move(source));
        assert(fixture->interval == 100);
        const auto revision = lv_subject_get_int(vm.revision_subject());
        vm.tick();
        assert(lv_subject_get_int(vm.revision_subject()) > revision);
        assert(!vm.showing_detail());
        vm.open_sensor("missing");
        assert(!vm.showing_detail());
        vm.open_sensor("accel");
        assert(!vm.showing_detail());
        vm.open_sensor("imu");
        assert(vm.showing_detail());
        assert(vm.model().selected_device().readings.size() == 2);
        const auto selection_revision = lv_subject_get_int(vm.revision_subject());
        vm.set_channel_recording_enabled("accel", 1, false);
        vm.set_channel_recording_enabled("gyro", 0, false);
        assert(lv_subject_get_int(vm.revision_subject()) > selection_revision);
        assert(vm.model().recording_channel_count() == 4);
        assert(!vm.model().channel_recording_enabled("accel", 1));
        assert(!vm.model().channel_recording_enabled("gyro", 0));
        assert(vm.model().channel_recording_enabled("accel", 0));
        assert(vm.model().channel_recording_enabled("gyro", 1));
        vm.show_list();
        assert(!vm.showing_detail());
        assert(vm.model().selected_id() == "imu");
        assert(vm.model().selected()->available);
        assert(vm.model().history()->points.size() == 1);
        vm.tick();
        assert(vm.model().history()->points.size() == 1);
        vm.set_interval(1000);
        assert(fixture->interval == 1000);
        vm.set_interval(55);
        assert(vm.interval() == 1000);
        vm.toggle_pause();
        assert(vm.model().paused());
        const auto paused_revision = lv_subject_get_int(vm.revision_subject());
        vm.tick();
        assert(lv_subject_get_int(vm.revision_subject()) == paused_revision);
        vm.rescan();
        assert(!vm.model().paused());
        assert(fixture->rescans == 1);
        fixture->data.sequence = 2;
        fixture->data.timestamp = Clock::now() - std::chrono::seconds(5);
        vm.tick();
        assert(!vm.model().selected()->available);
        assert(vm.model().selected()->error == "Sensor data is stale");
        fixture->data.sequence = 3;
        fixture->data.timestamp = Clock::now();
        vm.tick();
        assert(vm.model().selected()->available);
        vm.clear_history();
        assert(vm.model().history() == nullptr);
        vm.set_filter(2);
        assert(vm.model().selected() == nullptr);
        vm.set_filter(-1);
        assert(vm.model().selected() != nullptr);
        assert(!vm.model().channel_recording_enabled("accel", 1));
        assert(!vm.model().channel_recording_enabled("gyro", 0));
        const auto directory = std::filesystem::temp_directory_path() /
                               ("sensors-vm-test-" + std::to_string(getpid()));
        setenv("LILYGO_SENSORS_RECORD_DIR", directory.c_str(), 1);
        vm.toggle_recording();
        assert(vm.model().recording());
        ++fixture->data.sequence;
        vm.tick();
        assert(vm.model().recorded_rows() == 4);
        const auto recording_path = vm.model().recording_path();
        vm.set_channel_recording_enabled("accel", 1, true);
        assert(vm.model().recorded_rows() == 4);
        assert(vm.model().recording_path() == recording_path);
        ++fixture->data.sequence;
        vm.tick();
        assert(vm.model().recorded_rows() == 9);
        vm.toggle_recording();
        assert(!vm.model().recording());
        std::filesystem::remove_all(directory);
        unsetenv("LILYGO_SENSORS_RECORD_DIR");
        vm.open_sensor("imu");
        assert(vm.showing_detail());
        fixture->data.sensors.erase(fixture->data.sensors.begin());
        ++fixture->data.sequence;
        vm.tick();
        assert(vm.showing_detail());
        assert(vm.model().selected_id() == "imu" && vm.model().selected()->id == "gyro");
        assert(vm.model().selected_device().readings.size() == 1);
        auto replacement = fixture->data.sensors.front();
        replacement.id = "replacement";
        replacement.device_id = "replacement-device";
        fixture->data.sensors = {replacement};
        ++fixture->data.sequence;
        vm.tick();
        assert(!vm.showing_detail());
        assert(vm.model().selected_id() == "replacement-device");
        vm.open_sensor("replacement-device");
        assert(vm.showing_detail());
        vm.set_filter(2);
        assert(!vm.showing_detail());
    }
    lv_deinit();
    std::cout << "view model commands and stale data tests passed\n";
}
