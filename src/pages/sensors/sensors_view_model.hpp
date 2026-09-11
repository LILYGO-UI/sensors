#pragma once

#include "components/lv_subject.hpp"
#include "pages/sensors/sensors_model.hpp"

#include <memory>

namespace sensors {

class SensorsViewModel {
public:
    explicit SensorsViewModel(std::unique_ptr<SensorSource> source = make_sensor_source());
    void tick();
    void select(const std::string &id);
    void open_sensor(const std::string &id);
    void show_list();
    bool showing_detail() const { return !detail_id_.empty(); }
    void set_filter(int bus);
    void toggle_pause();
    void clear_history();
    void rescan();
    void set_interval(unsigned milliseconds);
    void set_channel_recording_enabled(const std::string &reading_id, std::size_t channel_index, bool enabled);
    void toggle_recording();
    unsigned interval() const { return interval_; }
    const SensorsModel &model() const { return model_; }
    lv_subject_t *revision_subject();

private:
    void publish();
    void reconcile_detail();
    SensorsModel model_;
    std::unique_ptr<SensorSource> source_;
    IntSubject revision_{0};
    unsigned interval_ = 100;
    std::string detail_id_;
};

}  // namespace sensors
