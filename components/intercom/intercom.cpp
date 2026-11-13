#include "intercom.h"
#include "esphome/core/log.h"
#include <cmath>  // NAN (clear the Decoder id when the ring stops)

namespace esphome {
namespace intercom {

static const char *const TAG = "intercom";

// ---------------------------------------------------------------------------
// Persisted, UI-editable entities
// ---------------------------------------------------------------------------
void IntercomNumber::restore() {
  this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
  float v;
  if (!this->pref_.load(&v))
    v = this->default_;
  this->publish_state(v);
}
void IntercomNumber::control(float value) {
  this->publish_state(value);
  this->pref_.save(&value);
}

void IntercomSwitch::restore() {
  this->pref_ = global_preferences->make_preference<bool>(this->get_object_id_hash());
  bool v;
  if (!this->pref_.load(&v))
    v = this->default_;
  this->publish_state(v);
}
void IntercomSwitch::write_state(bool state) {
  this->publish_state(state);
  this->pref_.save(&state);
}

void IntercomSelect::restore() {
  this->pref_ = global_preferences->make_preference<uint8_t>(this->get_object_id_hash());
  uint8_t i;
  if (!this->pref_.load(&i))
    i = this->default_index_;
  auto opt = this->at(i);
  if (opt.has_value())
    this->publish_state(*opt);
}
void IntercomSelect::control(const std::string &value) {
  auto idx = this->index_of(value);
  if (!idx.has_value())
    return;
  this->publish_state(value);
  uint8_t i = (uint8_t) *idx;
  this->pref_.save(&i);
}

void OpenDoorButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->open_door();
}

void PickupButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->pickup();
}

void HangupButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->hangup_line();
}

// ---------------------------------------------------------------------------
// Bus decode ISR: record the signed duration of the segment that just ended.
// ---------------------------------------------------------------------------
void IRAM_ATTR IntercomReceiver::gpio_isr(IntercomReceiver *self) {
  const uint32_t now = micros();
  const uint32_t dur = now - self->last_edge_us_;
  self->last_edge_us_ = now;
  const bool now_high = self->isr_pin_.digital_read();
  const int32_t signed_dur = now_high ? -(int32_t) dur : (int32_t) dur;  // the ended segment held !now
  const uint32_t next = (self->head_ + 1) % INTERCOM_EDGE_BUF;
  if (next != self->tail_) {
    self->edges_[self->head_] = signed_dur;
    self->head_ = next;
  }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void IntercomReceiver::setup() {
  // Actuator outputs, driven to the released state at boot.
  if (this->offhook_pin_ != nullptr) {
    this->offhook_pin_->setup();
    this->drive_offhook_(false);
  }
  if (this->mute_pin_ != nullptr) {
    this->mute_pin_->setup();
    this->drive_mute_(false);
  }

  // Bus decode interrupt.
  this->bus_pin_->setup();
  this->isr_pin_ = this->bus_pin_->to_isr();
  this->last_edge_us_ = micros();
  this->bus_pin_->attach_interrupt(&IntercomReceiver::gpio_isr, this, gpio::INTERRUPT_ANY_EDGE);

  // Hardware ring line (optional). Seed state so boot doesn't look like an edge.
  if (this->ring_pin_ != nullptr) {
    this->ring_pin_->setup();
    const bool lvl = this->ring_pin_->digital_read();
    this->ring_state_ = this->ring_inverted_ ? !lvl : lvl;
  }

  // Restore all persisted config entities (value from flash, else YAML default).
  if (this->open_apartment_ != nullptr)
    this->open_apartment_->restore();
  if (this->auto_on_ring_ != nullptr)
    this->auto_on_ring_->restore();
  for (Schedule *sch : {&this->auto_sched_, &this->mute_sched_}) {
    if (sch->mode != nullptr)
      sch->mode->restore();
    if (sch->from_h != nullptr)
      sch->from_h->restore();
    if (sch->until_h != nullptr)
      sch->until_h->restore();
    for (int i = 0; i < 7; i++)
      if (sch->days[i] != nullptr)
        sch->days[i]->restore();
  }

  // Momentary call indicators start Clear (not Unknown) until a call/ring pulses them.
  if (this->ring_sensor_ != nullptr)
    this->ring_sensor_->publish_state(false);
  if (this->decoding_successful_ != nullptr)
    this->decoding_successful_->publish_state(false);
  if (this->open_with_code_ != nullptr)
    this->open_with_code_->publish_state(false);

  // Scheduler: re-evaluate mute + auto-open now and every minute. Also re-evaluate immediately
  // whenever a mode select changes, so the output + "… active"/"… source" track it at once.
  if (this->mute_sched_.mode != nullptr)
    this->mute_sched_.mode->add_on_state_callback([this](size_t) { this->refresh_mute_(); });
  if (this->auto_sched_.mode != nullptr)
    this->auto_sched_.mode->add_on_state_callback([this](size_t) { this->refresh_auto_(); });
  this->set_interval("sched", 60000, [this]() { this->scheduler_tick_(); });
  this->scheduler_tick_();

  ESP_LOGCONFIG(TAG, "intercom controller up");
}

// ---------------------------------------------------------------------------
// Loop: drain decode edges, finalize frames, poll the ring line.
// ---------------------------------------------------------------------------
void IntercomReceiver::loop() {
  // Drain ISR edges into the current burst.
  while (this->tail_ != this->head_) {
    const int32_t v = this->edges_[this->tail_];
    this->tail_ = (this->tail_ + 1) % INTERCOM_EDGE_BUF;
    if ((uint32_t) abs(v) >= INTERCOM_MIN_EDGE_US && this->pulses_.size() < INTERCOM_MAX_PULSES)
      this->pulses_.push_back(v);
  }

  // Burst start: any bus activity means a ring is present on the line (for ANY apartment).
  if (!this->pulses_.empty() && !this->ring_active_) {
    this->ring_active_ = true;
    this->burst_decoded_ = false;
    this->ring_start_ms_ = millis();
    if (this->ring_sensor_ != nullptr)
      this->ring_sensor_->publish_state(true);
  }

  // Once the id preamble completes, report the targeted apartment. These indicators then stay
  // on for the rest of the ring and are cleared together at burst end (below).
  if (this->ring_active_ && !this->burst_decoded_) {
    IntercomFrame f = this->decode_basic_(this->pulses_, true);  // require a completed preamble
    if (f.valid) {
      this->burst_decoded_ = true;
      ESP_LOGI(TAG, "Call received: id=%u, code_open=%s", f.id, f.code_open ? "true" : "false");
      this->report_decode_(f);
    }
  }

  // Burst end: the bus has been idle longer than frame_gap AND the ring has been on for at
  // least ring_min_hold — then the ring has stopped.
  if (this->ring_active_ && (micros() - this->last_edge_us_) > this->frame_gap_us_ &&
      (millis() - this->ring_start_ms_) >= this->ring_min_hold_ms_) {
    if (this->dump_raw_) {
      std::string s;
      for (int32_t v : this->pulses_) {
        char b[14];
        snprintf(b, sizeof(b), "%ld, ", (long) v);
        s += b;
      }
      ESP_LOGI(TAG, "Dump Raw: %s", s.c_str());
    }
    // Short preamble-only ring (no trailing tone): decode once at the end if we haven't yet.
    if (!this->burst_decoded_) {
      IntercomFrame f = this->decode_basic_(this->pulses_, false);
      if (f.valid) {
        ESP_LOGI(TAG, "Call received: id=%u, code_open=%s", f.id, f.code_open ? "true" : "false");
        this->report_decode_(f);
      }
    }
    // Ring stopped: clear the call indicators.
    this->clear_call_indicators_();
    this->ring_active_ = false;
    this->pulses_.clear();
  }

  this->poll_ring_();
}

// id = number of short HIGH pulses before the first long LOW gap (verified: N pulses -> id N).
// require_terminator=true demands the preamble-ending long gap be present (safe mid-ring decode);
// false trusts the accumulated count at burst end (handles a preamble-only ring).
IntercomFrame IntercomReceiver::decode_basic_(const std::vector<int32_t> &pulses, bool require_terminator) {
  IntercomFrame f;
  uint32_t count = 0;
  bool started = false;
  bool terminated = false;
  for (int32_t v : pulses) {
    if (v > 0 && v < INTERCOM_PREAMBLE_PULSE_US) {  // short HIGH pulse
      count++;
      started = true;
    } else if (v < -INTERCOM_PREAMBLE_GAP_US && started) {
      terminated = true;
      break;  // long LOW gap ends the id preamble
    }
  }
  if (require_terminator && !terminated)
    return f;  // preamble not complete yet — don't trust a partial count
  if (count >= 1 && count <= 255) {
    f.id = count;
    f.code_open = false;  // encoding not yet confirmed
    f.valid = true;
  }
  return f;
}

// Publish the decoded apartment id + "Decoding successful" (+ Open with code). Held on until the
// ring stops (cleared in loop() at burst end), so it points at the apartment for the whole ring.
void IntercomReceiver::report_decode_(const IntercomFrame &f) {
  if (this->decoder_ != nullptr)
    this->decoder_->publish_state((float) f.id);
  if (this->decoding_successful_ != nullptr)
    this->decoding_successful_->publish_state(true);
  if (this->open_with_code_ != nullptr)
    this->open_with_code_->publish_state(f.code_open);
  this->maybe_auto_open_(false, f.id);
}

// Clear all call indicators: Doorbell (RING), Decoding successful, Open with code, and the id.
void IntercomReceiver::clear_call_indicators_() {
  if (this->ring_sensor_ != nullptr)
    this->ring_sensor_->publish_state(false);
  if (this->decoding_successful_ != nullptr)
    this->decoding_successful_->publish_state(false);
  if (this->open_with_code_ != nullptr)
    this->open_with_code_->publish_state(false);
  if (this->decoder_ != nullptr)
    this->decoder_->publish_state(NAN);  // apartment id is shown only while a call is active
}

// ---------------------------------------------------------------------------
// Hardware ring line (GPIO14): optional auto-open trigger on its rising edge. The Doorbell
// (RING) sensor is driven by bus activity (in loop()), not by this pin.
// ---------------------------------------------------------------------------
void IntercomReceiver::poll_ring_() {
  if (this->ring_pin_ == nullptr)
    return;
  const bool lvl = this->ring_pin_->digital_read();
  const bool active = this->ring_inverted_ ? !lvl : lvl;
  if (active == this->ring_state_)
    return;
  const uint32_t now = millis();
  if (now - this->ring_change_ms_ < 20)  // 20ms debounce
    return;
  this->ring_change_ms_ = now;
  this->ring_state_ = active;
  if (active)
    this->maybe_auto_open_(true, 0);
}

// ---------------------------------------------------------------------------
// Scheduler: window check + 3-way mode (Off / On / Schedule) + consumers
// ---------------------------------------------------------------------------
// True if the current time is inside the schedule window (clock-valid + day + hour). Ignores mode.
bool IntercomReceiver::in_window_(const Schedule &s) {
  if (this->time_ == nullptr)
    return false;
  ESPTime now = this->time_->now();
  if (!now.is_valid())
    return false;
  if (s.from_h == nullptr || s.until_h == nullptr)
    return false;
  const int h = now.hour;
  const int from = (int) s.from_h->state;
  const int until = (int) s.until_h->state;
  // [from, until); wraps past midnight when from > until (e.g. 22 -> 7).
  const bool wraps = from > until;
  const bool in_hours = wraps ? (h >= from || h < until) : (h >= from && h < until);
  if (!in_hours)
    return false;
  // ESPTime day_of_week: 1=Sun .. 7=Sat  ->  index 0=Mon .. 6=Sun
  const int dow = now.day_of_week;
  int idx = (dow == 1) ? 6 : (dow - 2);
  // The post-midnight tail (h < until) of a wrapping window belongs to the day the window
  // began, i.e. the previous calendar day — so a Fri 22->7 window stays "Friday" until 07:00 Sat.
  if (wraps && h < until)
    idx = (idx + 6) % 7;
  if (idx < 0 || idx > 6 || s.days[idx] == nullptr || !s.days[idx]->state)
    return false;
  return true;
}

// Effective state from the mode select (options index: 0=Off, 1=On, 2=Schedule).
bool IntercomReceiver::effective_(const Schedule &s) {
  if (s.mode == nullptr)
    return false;
  auto idx = s.mode->active_index();
  if (!idx.has_value())
    return false;
  if (*idx == 1)
    return true;   // On
  if (*idx == 0)
    return false;  // Off
  return this->in_window_(s);  // Schedule
}

// Which selector position is driving the feature (independent of on/off):
// "UserOff" (mode Off), "UserOn" (mode On), "Schedule" (mode Schedule). The resulting
// on/off is the "… active" sensor.
const char *IntercomReceiver::source_str_(const Schedule &s) {
  if (s.mode == nullptr)
    return "Schedule";
  auto idx = s.mode->active_index();  // 0=Off, 1=On, 2=Schedule
  if (!idx.has_value())
    return "Schedule";
  if (*idx == 0)
    return "UserOff";
  if (*idx == 1)
    return "UserOn";
  return "Schedule";
}

void IntercomReceiver::maybe_auto_open_(bool from_ring, uint32_t decoded_id) {
  if (this->pickup_active_)  // manual pickup holds the line; don't let auto-open clobber it
    return;
  if (!this->effective_(this->auto_sched_))  // auto-open must be "armed" (On or in-window)
    return;
  bool trig = false;
  if (from_ring) {
    if (this->auto_on_ring_ != nullptr && this->auto_on_ring_->state)
      trig = true;
  } else if (this->open_apartment_ != nullptr) {
    const int ap = (int) this->open_apartment_->state;
    if (ap != 0 && (int) decoded_id == ap)
      trig = true;
  }
  if (trig) {
    ESP_LOGI(TAG, "Auto-open triggered (%s)", from_ring ? "ring" : "apartment id");
    this->open_door();
  }
}

// Re-evaluate mute: drive GPIO12 and publish the live "Mute active" + "Mute source".
void IntercomReceiver::refresh_mute_() {
  const bool active = this->effective_(this->mute_sched_);
  this->drive_mute_(active);
  if (this->mute_active_ != nullptr)
    this->mute_active_->publish_state(active);
  if (this->mute_source_ != nullptr)
    this->mute_source_->publish_state(this->source_str_(this->mute_sched_));
}

// Re-evaluate auto-open and publish the live "Auto open active" + "Auto open source".
void IntercomReceiver::refresh_auto_() {
  const bool active = this->effective_(this->auto_sched_);
  if (this->auto_active_ != nullptr)
    this->auto_active_->publish_state(active);
  if (this->auto_source_ != nullptr)
    this->auto_source_->publish_state(this->source_str_(this->auto_sched_));
}

void IntercomReceiver::scheduler_tick_() {
  this->refresh_mute_();
  this->refresh_auto_();
}

// ---------------------------------------------------------------------------
// Actuator drive + non-blocking open-door hook-flash sequencer
// ---------------------------------------------------------------------------
void IntercomReceiver::drive_offhook_(bool on) {
  if (this->offhook_pin_ != nullptr)
    this->offhook_pin_->digital_write(this->offhook_inverted_ ? !on : on);
}
void IntercomReceiver::drive_mute_(bool on) {
  if (this->mute_pin_ != nullptr)
    this->mute_pin_->digital_write(this->mute_inverted_ ? !on : on);
}

// Recovered door hook-flash: {level, dwell-before-next}. The panel opens on the brief
// OFF flashes embedded in the held off-hook state.
struct DoorStep {
  bool level;
  uint32_t ms;
};
static const DoorStep DOOR_SEQ[] = {
    {true, DOOR_OFFHOOK_MS},   // pick up (off-hook), hold 2000ms
    {false, DOOR_FLASH_MS},    // hook-flash OFF, 10ms
    {true, DOOR_MID_MS},       // ON, 500ms
    {false, DOOR_FLASH_MS},    // hook-flash OFF, 10ms
    {true, DOOR_HANGUP_MS},    // ON, hold 1000ms
    {false, 0},                // hang up (released)
};
static const uint8_t DOOR_SEQ_LEN = sizeof(DOOR_SEQ) / sizeof(DOOR_SEQ[0]);

void IntercomReceiver::open_door() {
  ESP_LOGI(TAG, "Open door: running hook-flash sequence");
  this->pickup_active_ = false;  // door sequence takes over the off-hook pin
  if (this->clear_on_open_) {
    this->clear_call_indicators_();
    if (this->ring_active_)
      this->burst_decoded_ = true;
  }
  this->door_idx_ = 0;
  this->door_step_();
}

void IntercomReceiver::pickup() {
  ESP_LOGI(TAG, "Pickup: going off-hook");
  this->pickup_active_ = true;
  this->cancel_timeout("door");  // abort any in-progress open_door() sequence
  this->drive_offhook_(true);
}

void IntercomReceiver::hangup_line() {
  ESP_LOGI(TAG, "Hang up: releasing off-hook");
  this->pickup_active_ = false;
  this->cancel_timeout("door");
  this->drive_offhook_(false);
}

void IntercomReceiver::door_step_() {
  const DoorStep &s = DOOR_SEQ[this->door_idx_];
  this->drive_offhook_(s.level);
  if (this->door_idx_ + 1 >= DOOR_SEQ_LEN)
    return;  // last step: leave released, done
  this->set_timeout("door", s.ms, [this]() {
    this->door_idx_++;
    this->door_step_();
  });
}

void IntercomReceiver::dump_config() {
  ESP_LOGCONFIG(TAG, "Intercom controller:");
  LOG_PIN("  Bus pin: ", this->bus_pin_);
  LOG_PIN("  Off-hook pin: ", this->offhook_pin_);
  LOG_PIN("  Mute pin: ", this->mute_pin_);
  LOG_PIN("  Ring pin: ", this->ring_pin_);
  ESP_LOGCONFIG(TAG, "  offhook_inverted=%s mute_inverted=%s ring_inverted=%s dump_raw=%s",
                YESNO(this->offhook_inverted_), YESNO(this->mute_inverted_), YESNO(this->ring_inverted_),
                YESNO(this->dump_raw_));
}

}  // namespace intercom
}  // namespace esphome
