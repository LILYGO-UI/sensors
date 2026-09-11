#include "app.hpp"
#include "pages/sensors/sensors_view.hpp"
#include <cm0/typography.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::vector<lv_color32_t> framebuffer;
int display_width = 0;
lv_point_t pointer_position{};
lv_indev_state_t pointer_state = LV_INDEV_STATE_RELEASED;

void flush_display(lv_display_t *display, const lv_area_t *area, std::uint8_t *pixels)
{
    const int width = area->x2 - area->x1 + 1;
    const auto *source = reinterpret_cast<const lv_color32_t *>(pixels);
    for (int y = area->y1; y <= area->y2; ++y) {
        const auto from = static_cast<std::size_t>(y - area->y1) * width;
        const auto to = static_cast<std::size_t>(y) * display_width + area->x1;
        assert(to + width <= framebuffer.size());
        std::copy_n(source + from, width, framebuffer.begin() + to);
    }
    lv_display_flush_ready(display);
}

void read_pointer(lv_indev_t *, lv_indev_data_t *data)
{
    data->point = pointer_position;
    data->state = pointer_state;
}

void click_at(lv_indev_t *pointer, int x, int y)
{
    pointer_position = {x, y};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    lv_tick_inc(25);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    lv_obj_update_layout(lv_screen_active());
}

void click(lv_indev_t *pointer, lv_obj_t *object)
{
    lv_obj_scroll_to_view_recursive(object, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    lv_area_t area{};
    lv_obj_get_coords(object, &area);
    for (auto *parent = lv_obj_get_parent(object); parent; parent = lv_obj_get_parent(parent)) {
        lv_area_t outer{};
        lv_obj_get_coords(parent, &outer);
        area.x1 = std::max(area.x1, outer.x1);
        area.y1 = std::max(area.y1, outer.y1);
        area.x2 = std::min(area.x2, outer.x2);
        area.y2 = std::min(area.y2, outer.y2);
    }
    assert(area.x1 <= area.x2 && area.y1 <= area.y2);
    click_at(pointer, (area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
    lv_obj_update_layout(lv_screen_active());
}

void assert_inside(lv_obj_t *object, lv_obj_t *parent)
{
    lv_area_t child{}, outer{};
    lv_obj_get_coords(object, &child);
    lv_obj_get_coords(parent, &outer);
    if (child.x1 < outer.x1 || child.y1 < outer.y1 || child.x2 > outer.x2 || child.y2 > outer.y2) {
        const char *child_name = lv_obj_get_name(object);
        const char *parent_name = lv_obj_get_name(parent);
        std::fprintf(stderr, "Object %s [%d,%d,%d,%d] is outside %s [%d,%d,%d,%d]\n",
                     child_name ? child_name : "<unnamed>", child.x1, child.y1, child.x2, child.y2,
                     parent_name ? parent_name : "<unnamed>", outer.x1, outer.y1, outer.x2, outer.y2);
    }
    assert(child.x1 >= outer.x1 && child.y1 >= outer.y1);
    assert(child.x2 <= outer.x2 && child.y2 <= outer.y2);
}

bool is_visible(lv_obj_t *object)
{
    for (auto *ancestor = object; ancestor; ancestor = lv_obj_get_parent(ancestor)) {
        if (lv_obj_has_flag(ancestor, LV_OBJ_FLAG_HIDDEN)) return false;
    }
    return true;
}

void assert_text_widths(lv_obj_t *object)
{
    for (uint32_t index = 0; index < lv_obj_get_child_count(object); ++index) {
        auto *child = lv_obj_get_child(object, index);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_check_type(child, &lv_label_class)) {
            assert(lv_obj_get_width(child) <= lv_obj_get_width(object));
        }
        assert_text_widths(child);
    }
}

bool write_ppm(const char *path, int width, int height)
{
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (const auto &pixel : framebuffer) {
        file.put(static_cast<char>(pixel.red));
        file.put(static_cast<char>(pixel.green));
        file.put(static_cast<char>(pixel.blue));
    }
    return file.good();
}

std::string read_text(const std::filesystem::path &path)
{
    std::ifstream file(path);
    assert(file);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

unsigned channel_rows(const std::string &csv, const std::string &reading,
                      const std::string &channel, bool errors_only = false,
                      const std::string &device = "QMI8658", const std::string &location = "i2c-1")
{
    // Match the fixed fixture fields, including quotes and delimiters.
    const auto fields = ",\"" + reading + "\",\"" + device + "\",\"" + location + "\",\"" + channel + "\",";
    std::istringstream rows(csv);
    unsigned count = 0;
    for (std::string row; std::getline(rows, row);) {
        if (row.find(fields) != std::string::npos &&
            (!errors_only || row.find(",error,") != std::string::npos)) ++count;
    }
    return count;
}

class TestSource final : public sensors::SensorSource {
public:
    sensors::Snapshot value;
    unsigned rescans = 0, interval = 0;
    const sensors::Clock::time_point origin = sensors::Clock::now();
    sensors::Snapshot snapshot() override
    {
        ++value.sequence;
        value.timestamp = origin + std::chrono::milliseconds(value.sequence * 100);
        for (auto &sensor : value.sensors) sensor.timestamp = value.timestamp;
        return value;
    }
    void rescan() override { ++rescans; }
    void set_interval(unsigned milliseconds) override { interval = milliseconds; }
};

class TestKeyboardSource final : public sensors::KeyboardSource {
public:
    sensors::KeyboardSnapshot value;
    unsigned snapshots = 0, rescans = 0, clears = 0, selections = 0;
    bool active = false;
    std::vector<bool> activations;

    sensors::KeyboardSnapshot snapshot() override
    {
        ++snapshots;
        return value;
    }
    void set_active(bool enabled) override
    {
        active = enabled;
        activations.push_back(enabled);
        value.monitoring = enabled && !value.devices.empty() && value.error.empty();
        ++value.sequence;
    }
    void select_device(const std::string &id) override
    {
        ++selections;
        value.selected_id = id;
        value.events.clear();
        value.pressed.clear();
        ++value.sequence;
    }
    void rescan() override { ++rescans; }
    void clear_history() override
    {
        ++clears;
        value.events.clear();
        ++value.sequence;
    }
    void event(unsigned code, int action, unsigned long long timestamp,
               std::vector<unsigned> held)
    {
        value.events.push_back({code, action, sensors::key_name(code), timestamp});
        value.pressed = std::move(held);
        ++value.sequence;
    }
};

sensors::KeyboardSnapshot keyboard_fixture()
{
    sensors::KeyboardSnapshot result;
    result.devices = {{"tca8418-event0", "tca8418", "/dev/input/event0", "i2c-1", "", true}};
    result.selected_id = result.devices.front().id;
    result.sequence = 1;
    return result;
}

sensors::Snapshot fixture()
{
    sensors::Snapshot result;
    result.simulated = true;
    result.buses = {{0, "External I2C", "", {{0x44, "Unknown I2C device", "", sensors::BusDeviceStatus::acknowledged},
                                           {0x68, "I2C device", "", sensors::BusDeviceStatus::busy}}},
                    {1, "i2c@fe5b0000", "", {{0x6b, "QMI8658", "qmi8658"}, {0x44, "SHT4x", "sht4x"}}},
                    {2, "i2c@fe5c0000", "", {{0x23, "BH1750", "bh1750"}}}};
    sensors::SensorReading accel;
    accel.id = "accel";
    accel.device_id = "accel";
    accel.name = "Accelerometer";
    accel.category = "accelerometer";
    accel.device = "QMI8658";
    accel.location = "i2c-1";
    accel.path = "/sys/bus/iio/devices/iio:device0";
    accel.available = true;
    accel.channels = {{"X", "m/s2", 1.25}, {"Y", "m/s2", -0.43}, {"Z", "m/s2", 9.81}};
    result.sensors.push_back(accel);
    sensors::SensorReading temperature;
    temperature.id = "temperature";
    temperature.device_id = "temperature";
    temperature.name = "SHT4x temperature";
    temperature.category = "temperature";
    temperature.device = "SHT4x";
    temperature.location = "i2c-1";
    temperature.path = "/sys/class/hwmon/hwmon1/temp1_input";
    temperature.available = true;
    temperature.channels = {{"Temperature", "C", 24.75}};
    result.sensors.push_back(temperature);
    auto light = temperature;
    light.id = "light";
    light.device_id = "light";
    light.name = "BH1750 light";
    light.category = "light";
    light.device = "BH1750";
    light.location = "i2c-2";
    light.path = "/sys/bus/iio/devices/iio:device1/in_illuminance_input";
    light.channels = {{"Light", "lx", 356.5}};
    result.sensors.push_back(light);
    auto cpu = temperature;
    cpu.id = "cpu";
    cpu.device_id = "cpu";
    cpu.name = "CPU temperature";
    cpu.device = "SoC thermal zone";
    cpu.location = "system";
    cpu.channels = {{"Temperature", "C", 47.2}};
    result.sensors.push_back(cpu);
    auto gyro = accel;
    gyro.id = "gyro";
    gyro.name = "Gyroscope";
    gyro.category = "gyroscope";
    gyro.channels = {{"X", "rad/s", 0.12}, {"Y", "rad/s", -0.08}, {"Z", "rad/s", 0.03}};
    result.sensors.push_back(gyro);
    auto chip_temperature = accel;
    chip_temperature.id = "chip_temperature";
    chip_temperature.name = "Chip temperature";
    chip_temperature.category = "temperature";
    chip_temperature.channels = {{"Temperature", "C", 31.25}};
    result.sensors.push_back(chip_temperature);
    return result;
}

sensors::Snapshot microphone_fixture()
{
    sensors::Snapshot result;
    result.simulated = true;
    sensors::SensorReading microphone;
    microphone.device_id = "microphone";
    microphone.category = "microphone";
    microphone.device = "Microphone (ES8389)";
    microphone.location = "system";
    microphone.path = "plughw:0,0";
    microphone.available = true;
    microphone.id = "microphone_waveform";
    microphone.name = "Raw waveform";
    microphone.waveform.resize(40);
    for (std::size_t index = 0; index < microphone.waveform.size(); ++index)
        microphone.waveform[index] = 0.5 * std::sin(index * 2 * std::acos(-1.0) / 10);
    result.sensors.push_back(microphone);
    microphone.id = "microphone_rms";
    microphone.name = "RMS level";
    microphone.waveform.clear();
    microphone.channels = {{"RMS", "dBFS", -35.5}};
    result.sensors.push_back(microphone);
    return result;
}
}  // namespace

int main(int argc, char **argv)
{
    assert(argc == 4);
    const int width = std::atoi(argv[1]), height = std::atoi(argv[2]);
    assert(width >= 320 && height >= 240);
    display_width = width;
    framebuffer.resize(static_cast<std::size_t>(width) * height);
    std::vector<lv_color32_t> draw_buffer(static_cast<std::size_t>(width) * 80);
    lv_init();
    auto *display = lv_display_create(width, height);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr,
                           static_cast<uint32_t>(draw_buffer.size() * sizeof(lv_color32_t)), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    lv_display_set_default(display);
    auto *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    auto source = std::make_unique<TestSource>();
    auto *input = source.get();
    input->value = fixture();
    auto keyboard_source = std::make_unique<TestKeyboardSource>();
    auto *keyboard_input = keyboard_source.get();
    keyboard_input->value = keyboard_fixture();
    auto view_model = std::make_unique<sensors::SensorsViewModel>(std::move(source));
    sensors::SensorsView view(*view_model, std::move(keyboard_source));
    auto *root = lv_screen_active();
    auto *surface = view.create(root, "Sensors");
    view_model->tick();
    lv_obj_update_layout(root);
    auto find = [root](const char *name) {
        auto *object = lv_obj_find_by_name(root, name);
        assert(object);
        return object;
    };
    auto *header = find("app_header");
    auto *back = find("sensors_back");
    auto *pause = find("sensors_pause");
    auto *clear = find("sensors_clear");
    auto *rescan = find("sensors_rescan");
    auto *record = find("sensors_record");
    auto *filter = find("sensors_filter");
    auto *interval = find("sensors_interval");
    auto chart = [&] { return find("sensors_chart"); };
    auto *body = find("sensors_body");
    auto *list_panel = find("sensors_list_panel");
    auto *list = find("sensors_list");
    auto *detail = find("sensors_detail");
    auto *navigation = find("sensors_navigation");
    auto assert_column = [](const std::vector<lv_obj_t *> &objects) {
        lv_area_t previous{};
        for (std::size_t index = 0; index < objects.size(); ++index) {
            auto *object = objects[index];
            lv_area_t area{};
            lv_obj_get_coords(object, &area);
            assert(lv_obj_get_width(object) >= lv_obj_get_content_width(lv_obj_get_parent(object)) - 2);
            if (index) {
                assert(area.x1 == previous.x1 && area.x2 == previous.x2);
                assert(area.y1 > previous.y2);
            }
            previous = area;
        }
    };
    auto assert_measurements_column = [&] {
        assert_column({find("sensor_measurement_accel"), find("sensor_measurement_gyro"),
                       find("sensor_measurement_chip_temperature")});
        assert_column({lv_obj_get_parent(chart()), lv_obj_get_parent(find("sensors_chart_1")),
                       lv_obj_get_parent(find("sensors_chart_2"))});
    };
    auto assert_list_page = [&] {
        assert(!view_model->showing_detail());
        assert(is_visible(body) && is_visible(list_panel));
        assert(is_visible(navigation) && is_visible(rescan));
        assert(is_visible(find("sensor_row_keyboard")));
        assert(!is_visible(detail));
        auto *hidden_chart = lv_obj_find_by_name(root, "sensors_chart");
        assert(!hidden_chart || !is_visible(hidden_chart));
        assert(!is_visible(back) && !is_visible(clear));
        for (auto *button : {pause, rescan, record}) {
            assert(is_visible(button));
            assert_inside(button, header);
            assert(lv_obj_get_width(button) >= 48 && lv_obj_get_height(button) >= 48);
        }
        assert_inside(list_panel, body);
    };
    auto assert_detail_page = [&] {
        assert(view_model->showing_detail());
        assert(is_visible(body) && is_visible(detail));
        assert(!is_visible(list_panel) && !is_visible(navigation));
        assert(!is_visible(rescan));
        for (auto *button : {back, pause, clear, record}) {
            assert(is_visible(button));
            assert_inside(button, header);
            assert(lv_obj_get_width(button) >= 48 && lv_obj_get_height(button) >= 48);
        }
        assert_inside(detail, body);
    };
    assert(lv_color_eq(lv_obj_get_style_bg_color(surface, LV_PART_MAIN), lv_color_hex(0xf2f2f7)));
    assert_list_page();
    assert_inside(filter, width < 480 ? find("sensors_sampling") : navigation);
    assert_inside(interval, find("sensors_sampling"));
    assert(std::string(lv_label_get_text(find("sensors_status"))) == "Demo");
    assert(lv_obj_get_style_text_font(find("sensors_status"), LV_PART_MAIN)->line_height >= 18);
    assert(lv_obj_get_style_text_font(filter, LV_PART_MAIN)->line_height >= 18);
    assert(lv_obj_get_style_text_font(interval, LV_PART_MAIN)->line_height >= 18);
    assert_text_widths(surface);
    auto *original_row = find("sensor_row_accel");
    assert(view_model->model().visible_devices().size() == 4);
    assert(lv_obj_find_by_name(root, "sensor_row_gyro") == nullptr);
    assert(lv_obj_find_by_name(root, "sensor_row_chip_temperature") == nullptr);
    for (int index = 0; index < 40; ++index) {
        auto &channels = input->value.sensors[0].channels;
        channels[0].value = std::sin(index * 0.2) * 3;
        channels[1].value = std::cos(index * 0.2) * 2;
        channels[2].value = 9.81 + std::sin(index * 0.3);
        view_model->tick();
    }
    assert(find("sensor_row_accel") == original_row);
    assert(view_model->model().history()->points.size() == 41);
    assert_list_page();
    lv_obj_update_layout(root);
    lv_refr_now(display);
    assert(write_ppm(argv[3], width, height));
    click(pointer, original_row);
    assert_detail_page();
    assert(is_visible(find("sensor_measurement_accel")));
    assert(is_visible(find("sensor_measurement_gyro")));
    assert(is_visible(find("sensor_measurement_chip_temperature")));
    assert(std::string(lv_label_get_text(find("sensor_value_3"))) == "0.12");
    assert(std::string(lv_label_get_text(find("sensor_value_6"))) == "31.25");
    assert(lv_chart_get_series_next(chart(), nullptr));
    assert(lv_chart_get_series_next(find("sensors_chart_1"), nullptr));
    assert(lv_chart_get_series_next(find("sensors_chart_2"), nullptr));
    assert_text_widths(detail);
    assert_measurements_column();
    lv_refr_now(display);
    auto detail_path = std::filesystem::path(argv[3]);
    detail_path.replace_filename(detail_path.stem().string() + "-detail.ppm");
    assert(write_ppm(detail_path.c_str(), width, height));
    lv_area_t first_plot{}, detail_content{};
    lv_obj_get_coords(lv_obj_get_parent(chart()), &first_plot);
    lv_obj_get_content_coords(detail, &detail_content);
    lv_obj_scroll_to_y(detail, lv_obj_get_scroll_y(detail) + first_plot.y1 - detail_content.y1,
                       LV_ANIM_OFF);
    lv_obj_update_layout(root);
    lv_refr_now(display);
    const auto chart_image = std::filesystem::path(argv[3]).replace_extension().string() + "-chart.ppm";
    assert(write_ppm(chart_image.c_str(), width, height));
    click(pointer, back);
    assert_list_page();
    click(pointer, pause);
    assert(view_model->model().paused());
    assert(std::string(lv_label_get_text(find("sensors_status"))) == "Paused");
    const auto paused_count = view_model->model().history()->points.size();
    view_model->tick();
    assert(view_model->model().history()->points.size() == paused_count);
    click(pointer, original_row);
    assert_detail_page();
    assert(is_visible(chart()));
    assert(view_model->model().selected_id() == "accel");
    assert(view_model->model().history()->points.size() == paused_count);
    click(pointer, back);
    assert_list_page();
    click(pointer, original_row);
    assert_detail_page();
    assert(view_model->model().history()->points.size() == paused_count);
    click(pointer, clear);
    assert(!view_model->model().history() || view_model->model().history()->points.empty());
    for (const auto *reading_id : {"gyro", "chip_temperature"}) {
        const auto *history = view_model->model().history(reading_id);
        assert(!history || history->points.empty());
    }
    click(pointer, back);
    assert_list_page();
    click(pointer, rescan);
    assert(input->rescans == 1 && !view_model->model().paused());
    lv_dropdown_set_selected(interval, 1);
    lv_obj_send_event(interval, LV_EVENT_VALUE_CHANGED, nullptr);
    assert(view_model->interval() == 250 && input->interval == 250);

    lv_dropdown_set_selected(filter, 2);
    lv_obj_send_event(filter, LV_EVENT_VALUE_CHANGED, nullptr);
    assert(view_model->model().filter() == 0);
    lv_dropdown_set_selected(filter, 4);
    lv_obj_send_event(filter, LV_EVENT_VALUE_CHANGED, nullptr);
    assert(view_model->model().filter() == 2);
    assert(view_model->model().selected_id() == "light");
    assert_list_page();
    click(pointer, find("sensor_row_light"));
    assert_detail_page();
    assert(std::string(lv_label_get_text(find("sensor_value_0"))) == "356.50");
    click(pointer, back);
    assert_list_page();
    assert(view_model->model().filter() == 2 && lv_dropdown_get_selected(filter) == 4);
    lv_dropdown_set_selected(filter, 1);
    lv_obj_send_event(filter, LV_EVENT_VALUE_CHANGED, nullptr);
    assert(view_model->model().selected_id() == "cpu");
    lv_dropdown_set_selected(filter, 0);
    lv_obj_send_event(filter, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_update_layout(root);

    auto *cpu_row = find("sensor_row_cpu");
    lv_obj_scroll_to_view_recursive(cpu_row, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    const auto list_scroll = lv_obj_get_scroll_y(list);
    if (height <= 480) assert(list_scroll > 0);
    click(pointer, cpu_row);
    assert_detail_page();
    assert(view_model->model().selected_id() == "cpu");
    click(pointer, back);
    assert_list_page();
    assert(lv_obj_get_scroll_y(list) == list_scroll);

    click(pointer, find("sensor_row_temperature"));
    assert(view_model->model().selected_id() == "temperature");
    assert_detail_page();
    click(pointer, back);
    assert_list_page();
    click(pointer, find("sensor_row_accel"));
    assert(view_model->model().selected_id() == "accel");
    assert_detail_page();
    click(pointer, back);
    assert_list_page();

    auto *tabs = find("sensors_tabs");
    lv_area_t tab_area{};
    lv_obj_get_coords(tabs, &tab_area);
    click_at(pointer, tab_area.x1 + lv_obj_get_width(tabs) * 3 / 4, (tab_area.y1 + tab_area.y2) / 2);
    assert(is_visible(find("sensors_buses")));
    assert(!is_visible(body) && !is_visible(detail) && !is_visible(back));
    auto *bus_zero = find("sensor_bus_0");
    bool responding = false, busy = false;
    for (unsigned index = 0; index < lv_obj_get_child_count(bus_zero); ++index) {
        auto *child = lv_obj_get_child(bus_zero, index);
        if (!lv_obj_check_type(child, &lv_label_class)) continue;
        const std::string text = lv_label_get_text(child);
        responding |= text.find("0x44   Unknown I2C device\nResponding / No kernel driver") != std::string::npos;
        busy |= text.find("0x68   I2C device\nAddress busy") != std::string::npos;
    }
    assert(responding && busy);
    assert_text_widths(find("sensors_buses"));
    lv_refr_now(nullptr);
    const auto bus_image = std::filesystem::path(argv[3]).replace_extension().string() + "-buses.ppm";
    assert(write_ppm(bus_image.c_str(), width, height));
    find("sensor_bus_1");
    find("sensor_bus_2");
    click_at(pointer, tab_area.x1 + lv_obj_get_width(tabs) / 4, (tab_area.y1 + tab_area.y2) / 2);
    assert_list_page();

    const auto record_directory = std::filesystem::temp_directory_path() / ("sensors-render-" + std::to_string(width) + "x" + std::to_string(height));
    setenv("LILYGO_SENSORS_RECORD_DIR", record_directory.c_str(), 1);
    click(pointer, record);
    assert(view_model->model().recording());
    click(pointer, find("sensor_row_accel"));
    assert_detail_page();
    assert(view_model->model().recording());
    for (unsigned index = 0; index < 7; ++index)
        assert(lv_obj_has_state(find(("sensor_record_channel_" + std::to_string(index)).c_str()), LV_STATE_CHECKED));
    click(pointer, find("sensor_record_channel_0"));
    assert(!lv_obj_has_state(find("sensor_record_channel_0"), LV_STATE_CHECKED));
    input->value.sensors[0].channels[0].value = 2.75;
    view_model->tick();
    assert(std::string(lv_label_get_text(find("sensor_value_0"))) == "2.75");
    assert(view_model->model().history("accel")->points.back().values[0] == 2.75);
    auto recorded = read_text(view_model->model().recording_path());
    assert(channel_rows(recorded, "accel", "X") == 0);
    assert(channel_rows(recorded, "accel", "Y") == 1);
    assert(channel_rows(recorded, "accel", "Z") == 1);
    assert(channel_rows(recorded, "gyro", "X") == 1);
    click(pointer, back);
    assert_list_page();
    click(pointer, find("sensor_row_accel"));
    assert_detail_page();
    assert(!lv_obj_has_state(find("sensor_record_channel_0"), LV_STATE_CHECKED));
    assert(lv_obj_has_state(find("sensor_record_channel_1"), LV_STATE_CHECKED));
    input->value.sensors[0].available = false;
    input->value.sensors[0].error = "I2C read failed";
    view_model->tick();
    recorded = read_text(view_model->model().recording_path());
    assert(channel_rows(recorded, "accel", "X") == 0);
    assert(channel_rows(recorded, "accel", "Y", true) == 1);
    assert(channel_rows(recorded, "accel", "Z", true) == 1);
    assert(!lv_obj_has_state(find("sensor_record_channel_0"), LV_STATE_CHECKED));
    input->value.sensors[0].available = true;
    input->value.sensors[0].error.clear();
    click(pointer, find("sensor_record_channel_0"));
    assert(lv_obj_has_state(find("sensor_record_channel_0"), LV_STATE_CHECKED));
    view_model->tick();
    click(pointer, record);
    assert(!view_model->model().recording());
    assert(std::filesystem::is_regular_file(view_model->model().recording_path()));
    recorded = read_text(view_model->model().recording_path());
    assert(channel_rows(recorded, "accel", "X") == 1);
    assert(channel_rows(recorded, "accel", "Y") == 3);
    assert(channel_rows(recorded, "accel", "Z") == 3);
    assert(recorded.find("gyro") != std::string::npos);
    assert(recorded.find("chip_temperature") != std::string::npos);
    assert(recorded.find("0.12") != std::string::npos);
    assert(recorded.find("31.25") != std::string::npos);
    lv_obj_send_event(pause, LV_EVENT_LONG_PRESSED, nullptr);
    assert(is_visible(find("sensors_tooltip")));
    lv_obj_send_event(pause, LV_EVENT_RELEASED, nullptr);
    assert(!is_visible(find("sensors_tooltip")));

    input->value.sensors[0].available = false;
    input->value.sensors[0].error = "I2C read failed: remote I/O error while reading accelerometer registers. Check device power, bus ownership and driver availability before rescanning.";
    view_model->tick();
    lv_obj_update_layout(root);
    assert(std::string(lv_label_get_text(find("sensor_value_0"))) == "--");
    assert(std::string(lv_label_get_text(find("sensor_value_3"))) == "0.12");
    assert(std::string(lv_label_get_text(find("sensor_value_6"))) == "31.25");
    assert(is_visible(find("sensors_detail_error")));
    assert_detail_page();
    assert_measurements_column();
    assert_text_widths(surface);
    click(pointer, back);
    assert_list_page();
    click(pointer, find("sensor_row_accel"));
    assert_detail_page();
    assert(is_visible(find("sensors_detail_error")));
    input->value.sensors[0].available = true;
    input->value.sensors[0].error.clear();
    view_model->tick();
    auto *series = lv_chart_get_series_next(chart(), nullptr);
    assert(series);
    auto *values = lv_chart_get_series_y_array(chart(), series);
    assert(std::find(values, values + lv_chart_get_point_count(chart()), LV_CHART_POINT_NONE) != values + lv_chart_get_point_count(chart()));

    for (int index = 0; index < 80; ++index) {
        auto &channels = input->value.sensors[0].channels;
        channels[0].value = std::sin(index * 0.15) * 3;
        channels[1].value = std::cos(index * 0.15) * 2;
        channels[2].value = 9.81 + std::sin(index * 0.25);
        view_model->tick();
    }
    lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    lv_obj_scroll_to_y(detail, 0, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    lv_refr_now(display);
    const auto contrast = std::count_if(framebuffer.begin(), framebuffer.end(), [](const auto &pixel) {
        return pixel.red < 100 && pixel.green < 160 && pixel.blue < 180;
    });
    assert(contrast > 1000);
    click(pointer, pause);
    const auto selected = view_model->model().selected_id();
    const auto points = view_model->model().history()->points.size();
    // Resize the caller's parent to verify the view uses actual available space.
    lv_obj_set_size(surface, width >= 760 ? 568 : 1000, height);
    lv_obj_update_layout(root);
    assert(view_model->model().selected_id() == selected && view_model->model().paused());
    assert(view_model->model().history()->points.size() == points);
    assert_detail_page();
    assert_measurements_column();
    assert_text_widths(surface);
    lv_obj_set_size(surface, width, height);
    lv_obj_update_layout(root);
    assert_measurements_column();
    click(pointer, back);
    assert_list_page();
    assert(view_model->model().selected_id() == selected && view_model->model().paused());
    assert(view_model->model().history()->points.size() == points);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    lv_obj_set_size(surface, width >= 760 ? 568 : 1000, height);
    lv_obj_update_layout(root);
    assert_list_page();
    assert(view_model->model().selected_id() == selected && view_model->model().paused());
    assert(view_model->model().history()->points.size() == points);
    assert_text_widths(surface);
    lv_obj_set_size(surface, width, height);
    lv_obj_update_layout(root);
    view_model->toggle_pause();

    click(pointer, find("sensor_row_accel"));
    assert_detail_page();
    input->value.sensors.erase(input->value.sensors.begin());
    view_model->tick();
    lv_obj_update_layout(root);
    assert_detail_page();
    assert(view_model->model().selected_id() == "accel");
    assert(lv_obj_find_by_name(root, "sensor_measurement_accel") == nullptr);
    assert(is_visible(find("sensor_measurement_gyro")));
    assert(is_visible(find("sensor_measurement_chip_temperature")));
    assert(std::string(lv_label_get_text(find("sensor_value_0"))) == "0.12");
    assert(std::string(lv_label_get_text(find("sensor_value_3"))) == "31.25");
    assert(lv_chart_get_series_next(chart(), nullptr));
    input->value.sensors.erase(std::remove_if(input->value.sensors.begin(), input->value.sensors.end(),
        [](const auto &reading) { return reading.device_id == "accel"; }), input->value.sensors.end());
    view_model->tick();
    lv_obj_update_layout(root);
    assert_list_page();
    assert(view_model->model().selected_id() != "accel");
    assert(lv_obj_find_by_name(root, "sensor_row_accel") == nullptr);
    click(pointer, find("sensor_row_temperature"));
    assert_detail_page();
    input->value.sensors.clear();
    input->value.buses.clear();
    input->value.simulated = false;
    input->value.error = "No sensors are accessible. I2C adapters may be unavailable or permission to read devices was denied.";
    view_model->tick();
    lv_obj_update_layout(root);
    assert(std::string(lv_label_get_text(find("sensors_status"))) == "Disconnected");
    assert_list_page();
    assert(is_visible(find("sensors_list_empty")));
    assert(!is_visible(find("sensors_detail_empty")));
    assert(lv_obj_get_height(body) >= 40);
    assert_text_widths(surface);
    view_model->open_sensor("missing");
    lv_obj_update_layout(root);
    assert_list_page();
    click(pointer, rescan);
    assert(input->rescans == 2);
    input->value = fixture();
    view_model->tick();
    lv_obj_update_layout(root);
    assert_list_page();
    click(pointer, find("sensor_row_accel"));
    assert_detail_page();
    click(pointer, back);
    assert_list_page();

    auto select_tab = [&](unsigned index) {
        assert(index < 2);
        lv_obj_get_coords(tabs, &tab_area);
        click_at(pointer, tab_area.x1 + lv_obj_get_width(tabs) * (2 * index + 1) / 4,
                 (tab_area.y1 + tab_area.y2) / 2);
    };
    auto update_keyboard = [&] {
        lv_tick_inc(150);
        lv_timer_handler();
        lv_obj_update_layout(root);
    };
    auto label_text = [&](const char *name) { return std::string(lv_label_get_text(find(name))); };
    auto assert_keyboard_page = [&] {
        assert(is_visible(find("keyboard_page")));
        assert(!is_visible(navigation) && !is_visible(tabs));
        assert(!is_visible(body) && !is_visible(find("sensors_buses")));
        for (auto *object : {record, filter, interval, find("sensors_sampling")}) {
            assert(!is_visible(object));
        }
        for (auto *button : {back, pause, clear, rescan}) {
            assert(is_visible(button));
            assert_inside(button, header);
            assert(lv_obj_get_width(button) >= 48 && lv_obj_get_height(button) >= 48);
        }
        assert(label_text("app_title") == "Keyboard");
        assert_inside(find("app_title"), header);
        assert_inside(find("sensors_status"), header);
        assert_inside(find("keyboard_page"), surface);
        assert_text_widths(surface);
    };

    assert(!keyboard_input->active && keyboard_input->snapshots == 0);
    const auto sensor_rescans = input->rescans;
    click(pointer, pause);
    assert(view_model->model().paused());
    lv_dropdown_set_selected(filter, 3);
    lv_obj_send_event(filter, LV_EVENT_VALUE_CHANGED, nullptr);
    assert(view_model->model().filter() == 1);
    lv_obj_scroll_to_view_recursive(find("sensor_row_keyboard"), LV_ANIM_OFF);
    lv_obj_update_layout(root);
    const auto keyboard_list_scroll = lv_obj_get_scroll_y(list);
    click(pointer, find("sensor_row_keyboard"));
    assert_keyboard_page();
    assert(keyboard_input->active && keyboard_input->snapshots > 0);
    assert(label_text("sensors_status") == "Listening");
    assert(label_text("keyboard_device_info") == "i2c-1 / /dev/input/event0");
    assert(label_text("keyboard_last_state") == "Waiting");
    assert(label_text("keyboard_pressed") == "None");
    assert(is_visible(find("keyboard_empty")) && !is_visible(find("keyboard_events")));

    keyboard_input->event(30, 1, 1000, {30});
    update_keyboard();
    assert(label_text("keyboard_last_key") == sensors::key_name(30));
    assert(label_text("keyboard_last_code") == "Code 30 / 0x01E");
    assert(label_text("keyboard_last_state") == "Down");
    assert(label_text("keyboard_pressed") == sensors::key_name(30));
    keyboard_input->event(30, 2, 1100, {30});
    update_keyboard();
    assert(label_text("keyboard_last_state") == "Repeat");
    keyboard_input->event(48, 1, 1200, {30, 48});
    update_keyboard();
    assert(label_text("keyboard_last_state") == "Down");
    assert(label_text("keyboard_pressed") == sensors::key_name(30) + " + " + sensors::key_name(48));
    assert(label_text("keyboard_event_count") == "EVENTS / 3");
    assert(!is_visible(find("keyboard_empty")) && is_visible(find("keyboard_events")));
    auto *key_events = find("keyboard_events");
    assert(lv_table_get_row_count(key_events) == 4);
    assert(std::string(lv_table_get_cell_value(key_events, 1, 0)) == "0.200");
    assert(std::string(lv_table_get_cell_value(key_events, 1, 1)) == sensors::key_name(48) + " / 48");
    assert(std::string(lv_table_get_cell_value(key_events, 1, 2)) == "Down");
    assert(std::string(lv_table_get_cell_value(key_events, 2, 2)) == "Repeat");
    assert_keyboard_page();
    lv_refr_now(display);
    const auto keyboard_image = std::filesystem::path(argv[3]).replace_extension().string() + "-keyboard.ppm";
    assert(write_ppm(keyboard_image.c_str(), width, height));

    keyboard_input->event(30, 0, 1300, {48});
    update_keyboard();
    assert(label_text("keyboard_last_state") == "Up");
    assert(label_text("keyboard_pressed") == sensors::key_name(48));
    assert(std::string(lv_table_get_cell_value(key_events, 1, 2)) == "Up");
    click(pointer, clear);
    assert(keyboard_input->clears == 1);
    assert(label_text("keyboard_last_key") == "--");
    assert(label_text("keyboard_event_count") == "EVENTS / 0");
    assert(label_text("keyboard_pressed") == sensors::key_name(48));
    assert(is_visible(find("keyboard_empty")) && !is_visible(key_events));

    click(pointer, pause);
    assert(!keyboard_input->active && view_model->model().paused());
    assert(label_text("sensors_status") == "Paused");
    click(pointer, back);
    assert_list_page();
    assert(view_model->model().filter() == 1 && lv_dropdown_get_selected(filter) == 3);
    assert(lv_obj_get_scroll_y(list) == keyboard_list_scroll);
    click(pointer, pause);
    assert(!view_model->model().paused() && !keyboard_input->active);
    const auto inactive_snapshots = keyboard_input->snapshots;
    update_keyboard();
    assert(keyboard_input->snapshots == inactive_snapshots);
    click(pointer, find("sensor_row_keyboard"));
    assert_keyboard_page();
    assert(!keyboard_input->active && label_text("sensors_status") == "Paused");
    click(pointer, pause);
    assert(keyboard_input->active && !view_model->model().paused());
    assert(label_text("sensors_status") == "Listening");
    click(pointer, rescan);
    assert(keyboard_input->rescans == 1 && input->rescans == sensor_rescans);

    keyboard_input->value.devices.push_back(
        {"tca8418-event1", "tca8418", "/dev/input/event1", "i2c-2", "", true});
    ++keyboard_input->value.sequence;
    update_keyboard();
    auto *keyboard_devices = find("keyboard_devices");
    lv_dropdown_set_selected(keyboard_devices, 1);
    lv_obj_send_event(keyboard_devices, LV_EVENT_VALUE_CHANGED, nullptr);
    assert(keyboard_input->selections == 1);
    assert(keyboard_input->value.selected_id == "tca8418-event1");
    assert(label_text("keyboard_device_info") == "i2c-2 / /dev/input/event1");

    keyboard_input->value.devices[1].available = false;
    keyboard_input->value.error = "Cannot open /dev/input/event1: Permission denied";
    keyboard_input->value.monitoring = false;
    ++keyboard_input->value.sequence;
    update_keyboard();
    assert(is_visible(find("keyboard_error")));
    assert(label_text("keyboard_error") == keyboard_input->value.error);
    assert(label_text("sensors_status") == "Input error");
    assert_keyboard_page();

    keyboard_input->value.devices.clear();
    keyboard_input->value.selected_id.clear();
    keyboard_input->value.error = "TCA8418 keyboard not found";
    ++keyboard_input->value.sequence;
    update_keyboard();
    assert(lv_obj_has_state(keyboard_devices, LV_STATE_DISABLED));
    assert(std::string(lv_dropdown_get_options(keyboard_devices)) == "No TCA8418 keyboard");
    assert(!is_visible(find("keyboard_device_info")));
    assert(label_text("keyboard_error") == "TCA8418 keyboard not found");
    assert_keyboard_page();

    keyboard_input->value = keyboard_fixture();
    keyboard_input->value.monitoring = true;
    update_keyboard();
    assert(!lv_obj_has_state(keyboard_devices, LV_STATE_DISABLED));
    assert(!is_visible(find("keyboard_error")));
    assert(label_text("sensors_status") == "Listening");
    click(pointer, back);
    assert(!keyboard_input->active);
    assert_list_page();
    select_tab(1);
    assert(is_visible(find("sensors_buses")) && !is_visible(find("keyboard_page")));
    select_tab(0);
    assert_list_page();

    // Keep microphone coverage independent of the existing sensor fixture and counts.
    lv_dropdown_set_selected(filter, 0);
    lv_obj_send_event(filter, LV_EVENT_VALUE_CHANGED, nullptr);
    const auto preceding_sequence = input->value.sequence;
    input->value = microphone_fixture();
    input->value.sequence = preceding_sequence;
    for (unsigned index = 0; index < 40; ++index) view_model->tick();
    lv_obj_update_layout(root);
    assert_list_page();
    assert(view_model->model().visible_devices().size() == 1);
    assert(view_model->model().visible_devices().front().readings.size() == 2);
    const char *microphone_ids[] = {"microphone_waveform", "microphone_rms"};
    const char *microphone_values[] = {"", "-35.50"};
    for (const auto *id : microphone_ids)
        assert(lv_obj_find_by_name(root, (std::string("sensor_row_") + id).c_str()) == nullptr);
    click(pointer, find("sensor_row_microphone"));
    assert_detail_page();
    assert(view_model->model().selected_id() == "microphone");
    assert(label_text("sensors_detail_name") == "Microphone (ES8389)");
    for (unsigned index = 0; index < 2; ++index) {
        assert(is_visible(find((std::string("sensor_measurement_") + microphone_ids[index]).c_str())));
        const auto suffix = index ? "_" + std::to_string(index) : "";
        auto *microphone_chart = find(("sensors_chart" + suffix).c_str());
        auto *microphone_series = lv_chart_get_series_next(microphone_chart, nullptr);
        assert(microphone_series && !lv_chart_get_series_next(microphone_chart, microphone_series));
        assert(lv_chart_get_point_count(microphone_chart) == (index == 0 ? 40 : 40));
        auto *chart_values = lv_chart_get_series_y_array(microphone_chart, microphone_series);
        if (index == 0) {
            assert(chart_values[0] == 5000);
            assert(chart_values[10] == 5000);
        } else {
            auto *value = find("sensor_value_0");
            assert(std::string(lv_label_get_text(value)) == microphone_values[index]);
            assert(label_text("sensors_chart_range_1").find("dBFS") != std::string::npos);
        }
        const auto range = label_text(("sensors_chart_range" + suffix).c_str());
        assert(range.find(index == 0 ? " FS" : " dBFS") != std::string::npos);
    }
    assert_column({find("sensor_measurement_microphone_waveform"), find("sensor_measurement_microphone_rms")});
    assert_column({lv_obj_get_parent(find("sensors_chart")), lv_obj_get_parent(find("sensors_chart_1"))});
    assert_text_widths(surface);
    lv_refr_now(display);
    const auto microphone_image = std::filesystem::path(argv[3]).replace_extension().string() + "-microphone.ppm";
    assert(write_ppm(microphone_image.c_str(), width, height));
    lv_obj_get_coords(lv_obj_get_parent(chart()), &first_plot);
    lv_obj_get_content_coords(detail, &detail_content);
    lv_obj_scroll_to_y(detail, lv_obj_get_scroll_y(detail) + first_plot.y1 - detail_content.y1, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    lv_refr_now(display);
    const auto microphone_chart_image = std::filesystem::path(argv[3]).replace_extension().string() + "-microphone-chart.ppm";
    assert(write_ppm(microphone_chart_image.c_str(), width, height));

    auto microphone_rows = [&](const std::string &csv, bool errors_only = false) {
        const auto &reading = input->value.sensors[1];
        return channel_rows(csv, reading.id, reading.channels.front().name, errors_only,
                            reading.device, reading.location);
    };
    click(pointer, record);
    assert(view_model->model().recording());
    assert(view_model->model().recording_channel_count() == 1);
    auto *checkbox = find("sensor_record_channel_0");
    assert(lv_obj_has_state(checkbox, LV_STATE_CHECKED));
    click(pointer, checkbox);
    assert(!lv_obj_has_state(checkbox, LV_STATE_CHECKED));
    assert(view_model->model().recording_channel_count() == 0);
    view_model->tick();
    recorded = read_text(view_model->model().recording_path());
    assert(microphone_rows(recorded) == 0);
    click(pointer, back);
    assert_list_page();
    click(pointer, find("sensor_row_microphone"));
    assert_detail_page();
    checkbox = find("sensor_record_channel_0");
    assert(!lv_obj_has_state(checkbox, LV_STATE_CHECKED));
    click(pointer, checkbox);
    assert(lv_obj_has_state(checkbox, LV_STATE_CHECKED));
    input->value.sensors[0].available = false;
    input->value.sensors[0].error = "Cannot open capture PCM: Permission denied";
    input->value.sensors[1].available = false;
    input->value.sensors[1].error = "Cannot open capture PCM: Permission denied";
    view_model->tick();
    lv_obj_update_layout(root);
    assert(is_visible(find("sensors_detail_error")));
    assert(label_text("sensors_detail_error").find("Cannot open capture PCM: Permission denied") != std::string::npos);
    assert(label_text("sensor_value_0") == "--");
    assert_text_widths(surface);
    recorded = read_text(view_model->model().recording_path());
    assert(microphone_rows(recorded, true) == 1);
    input->value.sensors[0].available = true;
    input->value.sensors[0].error.clear();
    input->value.sensors[0].waveform.resize(40, 0.25);
    input->value.sensors[1].available = true;
    input->value.sensors[1].error.clear();
    view_model->tick();
    assert(!is_visible(find("sensors_detail_error")));
    assert(label_text("sensor_value_0") == microphone_values[1]);
    click(pointer, record);
    assert(!view_model->model().recording());
    recorded = read_text(view_model->model().recording_path());
    assert(microphone_rows(recorded) == 2);
    click(pointer, back);
    assert_list_page();
    click(pointer, find("sensor_row_keyboard"));
    assert(keyboard_input->active);
    view.destroy();
    assert(!keyboard_input->active && !keyboard_input->activations.back());
    view.destroy();
    view_model.reset();
    std::filesystem::remove_all(record_directory);
    unsetenv("LILYGO_SENSORS_RECORD_DIR");
    lv_indev_delete(pointer);
    lv_display_delete(display);
    lilygo_ui_fonts_deinit();
    lv_deinit();
    return 0;
}
