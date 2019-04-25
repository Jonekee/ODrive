
#include "odrive_main.h"


Encoder::Encoder(STM32_Timer_t* counter, STM32_GPIO_t* index_gpio,
                 STM32_GPIO_t* hallA_gpio, STM32_GPIO_t* hallB_gpio, STM32_GPIO_t* hallC_gpio,
                 STM32_ADCChannel_t* adc_sincos_s, STM32_ADCChannel_t* adc_sincos_c,
                 Config_t& config) :
        counter_(counter),
        index_gpio_(index_gpio),
        hallA_gpio_(hallA_gpio),
        hallB_gpio_(hallB_gpio),
        hallC_gpio_(hallC_gpio),
        adc_sincos_s_(adc_sincos_s), adc_sincos_c_(adc_sincos_c),
        config_(config)
{
}

static void enc_index_cb_wrapper(void* ctx) {
    reinterpret_cast<Encoder*>(ctx)->enc_index_cb();
}

bool Encoder::init() {
    update_pll_gains();

    if (config_.pre_calibrated && (config_.mode == Encoder::MODE_HALL || config_.mode == Encoder::MODE_SINCOS)) {
        is_ready_ = true;
    }

    if (!counter_) {
        return false;
    }
    if (!counter_->init(0xffff /* period */, STM32_Timer_t::UP /* mode */)) {
        return false;
    }
    if (!counter_->config_encoder_mode(hallA_gpio_ /* Mx_ENC_A */, hallB_gpio_ /* Mx_ENC_B */)) {
        return false;
    }
    if (!counter_->start_encoder()) {
        return false;
    }
    set_idx_subscribe();

    return true;
}

void Encoder::set_error(Error_t error) {
    error_ |= error;
}

//--------------------
// Hardware Dependent
//--------------------

// Triggered when an encoder passes over the "Index" pin
// TODO: only arm index edge interrupt when we know encoder has powered up
// (maybe by attaching the interrupt on start search, synergistic with following)
void Encoder::enc_index_cb() {
    if (config_.use_index) {
        set_circular_count(0, false);
        if (config_.zero_count_on_find_idx)
            set_linear_count(0); // Avoid position control transient after search
        if (config_.pre_calibrated) {
            is_ready_ = true;
        } else {
            // We can't use the update_offset facility in set_circular_count because
            // we also set the linear count before there is a chance to update. Therefore:
            // Invalidate offset calibration that may have happened before idx search
            is_ready_ = false;
        }
        index_found_ = true;
    }

    // Disable interrupt
    index_gpio_->unsubscribe();
}

void Encoder::set_idx_subscribe(bool override_enable) {
    if (override_enable || (config_.use_index && !config_.find_idx_on_lockin_only)) {
        index_gpio_->init(GPIO_t::INPUT, GPIO_t::PULL_DOWN);
        index_gpio_->subscribe(true, false, enc_index_cb_wrapper, this);
    }

    if (!config_.use_index || config_.find_idx_on_lockin_only) {
        index_gpio_->unsubscribe();
    }
}

void Encoder::update_pll_gains() {
    pll_kp_ = 2.0f * config_.bandwidth;  // basic conversion to discrete time
    pll_ki_ = 0.25f * (pll_kp_ * pll_kp_); // Critically damped
}

void Encoder::check_pre_calibrated() {
    if (!is_ready_)
        config_.pre_calibrated = false;
    if (config_.mode == MODE_INCREMENTAL && !index_found_)
        config_.pre_calibrated = false;
}

// Function that sets the current encoder count to a desired 32-bit value.
void Encoder::set_linear_count(int32_t count) {
    // Disable interrupts to make a critical section to avoid race condition
    uint32_t prim = cpu_enter_critical();

    // Update states
    shadow_count_ = count;
    pos_estimate_ = (float)count;
    //Write hardware last
    counter_->htim.Instance->CNT = count;

    cpu_exit_critical(prim);
}

// Function that sets the CPR circular tracking encoder count to a desired 32-bit value.
// Note that this will get mod'ed down to [0, cpr)
void Encoder::set_circular_count(int32_t count, bool update_offset) {
    // Disable interrupts to make a critical section to avoid race condition
    uint32_t prim = cpu_enter_critical();

    if (update_offset) {
        config_.offset += count - count_in_cpr_;
        config_.offset = mod(config_.offset, config_.cpr);
    }

    // Update states
    count_in_cpr_ = mod(count, config_.cpr);
    pos_cpr_ = (float)count_in_cpr_;

    cpu_exit_critical(prim);
}

bool Encoder::run_index_search() {
    config_.use_index = true;
    index_found_ = false;
    if (!config_.idx_search_unidirectional && axis_->motor_.config_.direction == 0) {
        axis_->motor_.config_.direction = 1;
    }

    bool orig_finish_on_enc_idx = axis_->config_.lockin.finish_on_enc_idx;
    axis_->config_.lockin.finish_on_enc_idx = true;
    bool status = axis_->run_lockin_spin();
    axis_->config_.lockin.finish_on_enc_idx = orig_finish_on_enc_idx;
    return status;
}

bool Encoder::run_direction_find() {
    int32_t init_enc_val = shadow_count_;
    bool orig_finish_on_distance = axis_->config_.lockin.finish_on_distance;
    axis_->config_.lockin.finish_on_distance = true;
    axis_->motor_.config_.direction = 1; // Must test spin forwards for direction detect logic
    bool status = axis_->run_lockin_spin();
    axis_->config_.lockin.finish_on_distance = orig_finish_on_distance;

    if (status) {
        // Check response and direction
        if (shadow_count_ > init_enc_val + 8) {
            // motor same dir as encoder
            axis_->motor_.config_.direction = 1;
        } else if (shadow_count_ < init_enc_val - 8) {
            // motor opposite dir as encoder
            axis_->motor_.config_.direction = -1;
        } else {
            axis_->motor_.config_.direction = 0;
        }
    }

    return status;
}

// @brief Turns the motor in one direction for a bit and then in the other
// direction in order to find the offset between the electrical phase 0
// and the encoder state 0.
// TODO: Do the scan with current, not voltage!
bool Encoder::run_offset_calibration() {
    static const float start_lock_duration = 1.0f;
    static const float scan_omega = 4.0f * M_PI;
    static const float scan_distance = 16.0f * M_PI;
    const float scan_duration = scan_distance / scan_omega;

    // Require index found if enabled
    if (config_.use_index && !index_found_) {
        set_error(ERROR_INDEX_NOT_FOUND_YET);
        return false;
    }

    // We use shadow_count_ to do the calibration, but the offset is used by count_in_cpr_
    // Therefore we have to sync them for calibration
    shadow_count_ = count_in_cpr_;

    float voltage_magnitude;
    if (axis_->motor_.config_.motor_type == Motor::MOTOR_TYPE_HIGH_CURRENT)
        voltage_magnitude = axis_->motor_.config_.calibration_current * axis_->motor_.config_.phase_resistance;
    else if (axis_->motor_.config_.motor_type == Motor::MOTOR_TYPE_GIMBAL)
        voltage_magnitude = axis_->motor_.config_.calibration_current;
    else
        return false;

    // go to motor zero phase for start_lock_duration to get ready to scan
    uint32_t start_ms = get_ticks_ms();
    if (!axis_->motor_.arm_foc())
        return axis_->error_ |= Axis::ERROR_MOTOR_FAILED, false;
    axis_->run_control_loop([&](float dt){
        float t = (float)(get_ticks_ms() - start_ms) / 1000.f;
        if (!axis_->motor_.FOC_update(0.0f, voltage_magnitude, 0.0f, 0.0f, 1000, true))
            return false; // error set inside enqueue_voltage_timings
        return t > start_lock_duration;
    });
    if (axis_->error_ != Axis::ERROR_NONE)
        return false;

    int32_t init_enc_val = shadow_count_;
    int64_t encvaluesum = 0;
    uint64_t num_steps = 0;

    // scan forward
    start_ms = get_ticks_ms();
    axis_->run_control_loop([&](float dt){
        float t = (float)(get_ticks_ms() - start_ms) / 1000.f;
        float phase = wrap_pm_pi(scan_omega * t - scan_distance / 2.0f);
        if (!axis_->motor_.FOC_update(0.0f, voltage_magnitude, phase, scan_omega, 1000, true))
            return false; // error set inside enqueue_voltage_timings

        encvaluesum += shadow_count_;
        num_steps++;
        
        return t > scan_duration;
    });
    if (axis_->error_ != Axis::ERROR_NONE)
        return false;

    // Check response and direction
    if (shadow_count_ > init_enc_val + 8) {
        // motor same dir as encoder
        axis_->motor_.config_.direction = 1;
    } else if (shadow_count_ < init_enc_val - 8) {
        // motor opposite dir as encoder
        axis_->motor_.config_.direction = -1;
    } else {
        // Encoder response error
        set_error(ERROR_NO_RESPONSE);
        return false;
    }

    //TODO avoid recomputing elec_rad_per_enc every time
    // Check CPR
    float elec_rad_per_enc = axis_->motor_.config_.pole_pairs * 2 * M_PI * (1.0f / (float)(config_.cpr));
    float expected_encoder_delta = scan_distance / elec_rad_per_enc;
    float actual_encoder_delta_abs = fabsf(shadow_count_-init_enc_val);
    if(fabsf(actual_encoder_delta_abs - expected_encoder_delta)/expected_encoder_delta > config_.calib_range)
    {
        set_error(ERROR_CPR_OUT_OF_RANGE);
        return false;
    }

    // scan backwards
    start_ms = get_ticks_ms();
    axis_->run_control_loop([&](float dt){
        float t = (float)(get_ticks_ms() - start_ms) / 1000.f;
        float phase = wrap_pm_pi(-scan_omega * t + scan_distance / 2.0f);
        if (!axis_->motor_.FOC_update(0.0f, voltage_magnitude, phase, scan_omega, 1000, true))
            return false; // error set inside enqueue_voltage_timings

        encvaluesum += shadow_count_;
        num_steps++;
        
        return t > scan_duration;
    });
    if (axis_->error_ != Axis::ERROR_NONE)
        return false;

    config_.offset = encvaluesum / (num_steps * 2);
    int32_t residual = encvaluesum - ((int64_t)config_.offset * (int64_t)(num_steps * 2));
    config_.offset_float = (float)residual / (float)(num_steps * 2) + 0.5f; // add 0.5 to center-align state to phase

    is_ready_ = true;
    return true;
}

static bool decode_hall(uint8_t hall_state, int32_t* hall_cnt) {
    switch (hall_state) {
        case 0b001: *hall_cnt = 0; return true;
        case 0b011: *hall_cnt = 1; return true;
        case 0b010: *hall_cnt = 2; return true;
        case 0b110: *hall_cnt = 3; return true;
        case 0b100: *hall_cnt = 4; return true;
        case 0b101: *hall_cnt = 5; return true;
        default: return false;
    }
}

void Encoder::sample_now() {
    switch (config_.mode) {
        case MODE_INCREMENTAL: {
            tim_cnt_sample_ = (int16_t)counter_->htim.Instance->CNT;
        } break;

        case MODE_HALL: {
            // do nothing: samples already captured in general GPIO capture
        } break;

        case MODE_SINCOS: {
            float val_sin, val_cos;
            adc_sincos_s_->get_normalized(&val_sin);
            adc_sincos_c_->get_normalized(&val_cos);
            sincos_sample_s_ = val_sin - 0.5f;
            sincos_sample_c_ = val_cos - 0.5f;
        } break;

        default: {
           set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
        } break;
    }
}

void Encoder::decode_hall_samples(uint16_t GPIO_samples[n_GPIO_samples]) {
    STM32_GPIO_t* hall_gpios[] = {
        hallC_gpio_,
        hallB_gpio_,
        hallA_gpio_,
    };

    uint8_t hall_state = 0x0;
    for (int i = 0; i < 3; ++i) {
        if (!hall_gpios[i]) {
            continue;
        }

        int port_idx = 0;
        for (;;) {
            auto port = GPIOs_to_samp[port_idx];
            if (port == hall_gpios[i]->port)
                break;
            ++port_idx;
        }

        hall_state <<= 1;
        hall_state |= (GPIO_samples[port_idx] & (1U << hall_gpios[i]->pin_number)) ? 1 : 0;
    }

    hall_state_ = hall_state;
}

bool Encoder::update(float dt) {
    // Check that we don't get problems with discrete time approximation
    if (!(dt * pll_kp_ < 1.0f)) {
        set_error(ERROR_UNSTABLE_GAIN);
    }

    // update internal encoder state.
    int32_t delta_enc = 0;
    switch (config_.mode) {
        case MODE_INCREMENTAL: {
            //TODO: use count_in_cpr_ instead as shadow_count_ can overflow
            //or use 64 bit
            int16_t delta_enc_16 = (int16_t)tim_cnt_sample_ - (int16_t)shadow_count_;
            delta_enc = (int32_t)delta_enc_16; //sign extend
        } break;

        case MODE_HALL: {
            int32_t hall_cnt;
            if (decode_hall(hall_state_, &hall_cnt)) {
                delta_enc = hall_cnt - count_in_cpr_;
                delta_enc = mod(delta_enc, 6);
                if (delta_enc > 3)
                    delta_enc -= 6;
            } else {
                if (!config_.ignore_illegal_hall_state) {
                    set_error(ERROR_ILLEGAL_HALL_STATE);
                    return false;
                }
            }
        } break;

        case MODE_SINCOS: {
            float phase = fast_atan2(sincos_sample_s_, sincos_sample_c_);
            int fake_count = (int)(1000.0f * phase);
            //CPR = 6283 = 2pi * 1k

            delta_enc = fake_count - count_in_cpr_;
            delta_enc = mod(delta_enc, 6283);
            if (delta_enc > 6283/2)
                delta_enc -= 6283;
        } break;
        
        default: {
           set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
           return false;
        } break;
    }

    shadow_count_ += delta_enc;
    count_in_cpr_ += delta_enc;
    count_in_cpr_ = mod(count_in_cpr_, config_.cpr);

    //// run pll (for now pll is in units of encoder counts)
    // Predict current pos
    pos_estimate_ += dt * vel_estimate_;
    pos_cpr_      += dt * vel_estimate_;
    // discrete phase detector
    float delta_pos     = (float)(shadow_count_ - (int32_t)floorf(pos_estimate_));
    float delta_pos_cpr = (float)(count_in_cpr_ - (int32_t)floorf(pos_cpr_));
    delta_pos_cpr = wrap_pm(delta_pos_cpr, 0.5f * (float)(config_.cpr));
    // pll feedback
    pos_estimate_ += dt * pll_kp_ * delta_pos;
    pos_cpr_      += dt * pll_kp_ * delta_pos_cpr;
    pos_cpr_ = fmodf_pos(pos_cpr_, (float)(config_.cpr));
    vel_estimate_      += dt * pll_ki_ * delta_pos_cpr;
    bool snap_to_zero_vel = false;
    if (fabsf(vel_estimate_) < 0.5f * dt * pll_ki_) {
        vel_estimate_ = 0.0f; //align delta-sigma on zero to prevent jitter
        snap_to_zero_vel = true;
    }

    //// run encoder count interpolation
    int32_t corrected_enc = count_in_cpr_ - config_.offset;
    // if we are stopped, make sure we don't randomly drift
    if (snap_to_zero_vel || !config_.enable_phase_interpolation) {
        interpolation_ = 0.5f;
    // reset interpolation if encoder edge comes
    } else if (delta_enc > 0) {
        interpolation_ = 0.0f;
    } else if (delta_enc < 0) {
        interpolation_ = 1.0f;
    } else {
        // Interpolate (predict) between encoder counts using vel_estimate,
        interpolation_ += dt * vel_estimate_;
        // don't allow interpolation indicated position outside of [enc, enc+1)
        if (interpolation_ > 1.0f) interpolation_ = 1.0f;
        if (interpolation_ < 0.0f) interpolation_ = 0.0f;
    }
    float interpolated_enc = corrected_enc + interpolation_;

    //// compute electrical phase
    //TODO avoid recomputing elec_rad_per_enc every time
    float elec_rad_per_enc = axis_->motor_.config_.pole_pairs * 2 * M_PI * (1.0f / (float)(config_.cpr));
    float ph = elec_rad_per_enc * (interpolated_enc - config_.offset_float);
    // ph = fmodf(ph, 2*M_PI);
    phase_ = wrap_pm_pi(ph);

    return true;
}
