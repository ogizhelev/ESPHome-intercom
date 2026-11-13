#pragma once
// intercom — ESPHome external component: self-contained ESP8266 apartment-intercom
// controller. It is the WHOLE device logic:

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/gpio.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/select/select.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/time/real_time_clock.h"
#include <vector>

namespace esphome {
namespace intercom {

// ---- bus decode timing (basic unary pulse-count) defaults ----
static const uint32_t INTERCOM_EDGE_BUF = 512;         // edge ring buffer
static const int32_t  INTERCOM_PREAMBLE_PULSE_US = 60; // a preamble "short high" is shorter than this
static const int32_t  INTERCOM_PREAMBLE_GAP_US = 2000; // low gap longer than this ends the id preamble
static const uint32_t INTERCOM_FRAME_GAP_US = 120000;  // idle longer than this finalizes the frame
static const uint32_t INTERCOM_MIN_EDGE_US = 5;        // noise floor
static const uint32_t INTERCOM_MAX_PULSES = 2000;      // sanity cap per frame


static const uint32_t DOOR_OFFHOOK_MS = 2000;  // hold off-hook before the flash
static const uint32_t DOOR_FLASH_MS = 10;      // brief hook-flash OFF pulse
static const uint32_t DOOR_MID_MS = 500;       // ON between the two flashes
static const uint32_t DOOR_HANGUP_MS = 1000;   // hold before hang up

class IntercomReceiver;  // fwd

// A UI-editable Number whose value is persisted in flash and defaults to the YAML value.
class IntercomNumber : public number::Number {
 public:
  void set_default_value(float d) { this->default_ = d; }
  void restore();                      // load persisted value (or YAML default) and publish
  void control(float value) override;  // UI change -> publish + persist
 protected:
  ESPPreferenceObject pref_;
  float default_{0};
};

// A UI-editable Switch whose value is persisted in flash and defaults to the YAML value.
class IntercomSwitch : public switch_::Switch {
 public:
  void set_default_value(bool d) { this->default_ = d; }
  void restore();                       // load persisted value (or YAML default) and publish
  void write_state(bool state) override;  // UI/user change -> publish + persist
 protected:
  ESPPreferenceObject pref_;
  bool default_{false};
};

// A UI-editable 3-way mode Select (Off / On / Schedule), persisted in flash.
class IntercomSelect : public select::Select {
 public:
  void set_default_index(uint8_t i) { this->default_index_ = i; }
  void restore();                              // load persisted index (or YAML default) and publish
  void control(const std::string &value) override;  // UI change -> publish + persist
 protected:
  ESPPreferenceObject pref_;
  uint8_t default_index_{0};
};

// "Open door" button -> runs the hook-flash sequence on the parent.
class OpenDoorButton : public button::Button {
 public:
  void set_parent(IntercomReceiver *p) { this->parent_ = p; }
 protected:
  void press_action() override;
  IntercomReceiver *parent_{nullptr};
};

// "Pickup" button -> goes off-hook and holds (opens the audio line without opening the door).
class PickupButton : public button::Button {
 public:
  void set_parent(IntercomReceiver *p) { this->parent_ = p; }
 protected:
  void press_action() override;
  IntercomReceiver *parent_{nullptr};
};

// "Hang up" button -> releases off-hook (ends the call without the door sequence).
class HangupButton : public button::Button {
 public:
  void set_parent(IntercomReceiver *p) { this->parent_ = p; }
 protected:
  void press_action() override;
  IntercomReceiver *parent_{nullptr};
};

struct IntercomFrame {
  uint32_t id{0};
  bool code_open{false};
  bool valid{false};
};

// One schedule window + mode (reused for auto-open and mute).
struct Schedule {
  IntercomSelect *mode{nullptr};  // Off / On / Schedule
  IntercomNumber *from_h{nullptr};
  IntercomNumber *until_h{nullptr};
  IntercomSwitch *days[7]{};  // index 0=Mon .. 6=Sun
};

class IntercomReceiver : public Component {
 public:
  // ---- codegen setters: pins & options ----
  void set_bus_pin(InternalGPIOPin *p) { bus_pin_ = p; }
  void set_ring_pin(InternalGPIOPin *p) { ring_pin_ = p; }
  void set_offhook_pin(GPIOPin *p) { offhook_pin_ = p; }
  void set_mute_pin(GPIOPin *p) { mute_pin_ = p; }
  void set_time(time::RealTimeClock *t) { time_ = t; }
  void set_offhook_inverted(bool v) { offhook_inverted_ = v; }
  void set_mute_inverted(bool v) { mute_inverted_ = v; }
  void set_ring_inverted(bool v) { ring_inverted_ = v; }
  void set_dump_raw(bool v) { dump_raw_ = v; }
  void set_frame_gap(uint32_t ms) { this->frame_gap_us_ = ms * 1000; }  // bus-idle time that ends a ring
  void set_ring_min_hold(uint32_t ms) { this->ring_min_hold_ms_ = ms; }  // min RING on-time per burst
  void set_clear_on_open(bool v) { this->clear_on_open_ = v; }           // clear indicators when door opens

  // ---- codegen setters: entities ----
  void set_ring_sensor(binary_sensor::BinarySensor *b) { ring_sensor_ = b; }
  void set_decoding_successful(binary_sensor::BinarySensor *b) { decoding_successful_ = b; }
  void set_open_with_code(binary_sensor::BinarySensor *b) { open_with_code_ = b; }
  void set_decoder_sensor(sensor::Sensor *s) { decoder_ = s; }
  void set_mute_active(binary_sensor::BinarySensor *b) { mute_active_ = b; }
  void set_auto_active(binary_sensor::BinarySensor *b) { auto_active_ = b; }
  void set_mute_source(text_sensor::TextSensor *t) { mute_source_ = t; }
  void set_auto_source(text_sensor::TextSensor *t) { auto_source_ = t; }
  void set_open_apartment(IntercomNumber *n) { open_apartment_ = n; }
  void set_auto_on_ring(IntercomSwitch *s) { auto_on_ring_ = s; }

  void set_auto_mode(IntercomSelect *s) { auto_sched_.mode = s; }
  void set_auto_from(IntercomNumber *n) { auto_sched_.from_h = n; }
  void set_auto_until(IntercomNumber *n) { auto_sched_.until_h = n; }
  void set_auto_day(uint8_t i, IntercomSwitch *s) { if (i < 7) auto_sched_.days[i] = s; }
  void set_mute_mode(IntercomSelect *s) { mute_sched_.mode = s; }
  void set_mute_from(IntercomNumber *n) { mute_sched_.from_h = n; }
  void set_mute_until(IntercomNumber *n) { mute_sched_.until_h = n; }
  void set_mute_day(uint8_t i, IntercomSwitch *s) { if (i < 7) mute_sched_.days[i] = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void open_door();    // public: full hook-flash door-open sequence
  void pickup();       // go off-hook and hold (opens audio line, no door sequence)
  void hangup_line();  // release off-hook (ends the call, no door sequence)

 protected:
  static void IRAM_ATTR gpio_isr(IntercomReceiver *self);
  IntercomFrame decode_basic_(const std::vector<int32_t> &pulses, bool require_terminator);
  void report_decode_(const IntercomFrame &f);  // publish id + Decoding successful (held until ring stops)
  void clear_call_indicators_();                 // clear RING + Decoding successful + Open with code + id
  void maybe_auto_open_(bool from_ring, uint32_t decoded_id);
  bool in_window_(const Schedule &s);   // time/day/hour in the schedule window (ignores mode)
  bool effective_(const Schedule &s);   // Off->false, On->true, Schedule->in_window_
  const char *source_str_(const Schedule &s);  // "UserOn" / "UserOff" / "Schedule" — the selector position
  void drive_offhook_(bool on);
  void drive_mute_(bool on);
  void door_step_();   // non-blocking sequencer tick
  void refresh_mute_();     // recompute mute window -> drive GPIO12 + publish "Mute active"
  void refresh_auto_();     // recompute auto-open window -> publish "Auto open active"
  void scheduler_tick_();   // periodic re-evaluation of both schedules
  void poll_ring_();

  // pins / options
  InternalGPIOPin *bus_pin_{nullptr};
  InternalGPIOPin *ring_pin_{nullptr};
  GPIOPin *offhook_pin_{nullptr};
  GPIOPin *mute_pin_{nullptr};
  time::RealTimeClock *time_{nullptr};
  bool offhook_inverted_{false};
  bool mute_inverted_{false};
  bool ring_inverted_{false};
  bool dump_raw_{false};
  bool clear_on_open_{true};
  uint32_t frame_gap_us_{INTERCOM_FRAME_GAP_US};
  uint32_t ring_min_hold_ms_{120};

  // entities
  binary_sensor::BinarySensor *ring_sensor_{nullptr};
  binary_sensor::BinarySensor *decoding_successful_{nullptr};
  binary_sensor::BinarySensor *open_with_code_{nullptr};
  sensor::Sensor *decoder_{nullptr};
  binary_sensor::BinarySensor *mute_active_{nullptr};
  binary_sensor::BinarySensor *auto_active_{nullptr};
  text_sensor::TextSensor *mute_source_{nullptr};
  text_sensor::TextSensor *auto_source_{nullptr};
  IntercomNumber *open_apartment_{nullptr};
  IntercomSwitch *auto_on_ring_{nullptr};
  Schedule auto_sched_{};
  Schedule mute_sched_{};

  // bus decode ISR state
  ISRInternalGPIOPin isr_pin_;
  volatile int32_t edges_[INTERCOM_EDGE_BUF];
  volatile uint32_t head_{0};
  volatile uint32_t tail_{0};  // read by the ISR (buffer-full test) -> must be volatile
  volatile uint32_t last_edge_us_{0};
  std::vector<int32_t> pulses_;
  bool ring_active_{false};    // a bus burst (any ring) is in progress
  bool burst_decoded_{false};  // the current burst already yielded a valid id
  uint32_t ring_start_ms_{0};  // millis() at burst start (for ring_min_hold)

  // ring line debounce
  bool ring_state_{false};
  uint32_t ring_change_ms_{0};

  // door sequencer
  uint8_t door_idx_{0};

  // true while the line is held via pickup(); blocks auto-open so the sequence doesn't hang up
  bool pickup_active_{false};
};

}  // namespace intercom
}  // namespace esphome
