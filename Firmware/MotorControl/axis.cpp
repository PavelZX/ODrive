
#include <stdlib.h>
#include <functional>
#include <stm32_gpio.hpp>

#include "utils.h"
#include "odrive_main.h"

Axis::Axis(Motor motor,
           Encoder encoder,
           SensorlessEstimator sensorless_estimator,
           Controller controller,
           TrapezoidalTrajectory trap,
           osPriority thread_priority,
           Config_t& config)
    : motor_(motor),
      encoder_(encoder),
      sensorless_estimator_(sensorless_estimator),
      controller_(controller),
      trap_(trap),
      thread_priority_(thread_priority),
      config_(config)
{
    motor_.axis_ = this;
    encoder_.axis_ = this;
    sensorless_estimator_.axis_ = this;
    controller_.axis_ = this;
    trap_.axis_ = this;
}

// @brief Sets up all components of the axis,
// such as gate driver and encoder hardware.
bool Axis::init() {
    if (!motor_.init())
        return false;
    if (!encoder_.init())
        return false;
    if (!sensorless_estimator_.init())
        return false;
    if (!controller_.init())
        return false;
    if (!trap_.init())
        return false;

    decode_step_dir_pins();
    update_watchdog_settings();
    return true;
}

static void run_state_machine_loop_wrapper(void* ctx) {
    reinterpret_cast<Axis*>(ctx)->run_state_machine_loop();
    reinterpret_cast<Axis*>(ctx)->thread_id_valid_ = false;
}

// @brief Starts run_state_machine_loop in a new thread
void Axis::start_thread() {
    osThreadDef(thread_def, run_state_machine_loop_wrapper, thread_priority_, 0, 4*512);
    thread_id_ = osThreadCreate(osThread(thread_def), this);
    thread_id_valid_ = true;
}

// @brief Unblocks the control loop thread.
// This is called from the current sense interrupt handler.
void Axis::signal_current_meas() {
    if (thread_id_valid_)
        osSignalSet(thread_id_, M_SIGNAL_PH_CURRENT_MEAS);
}

// @brief Blocks until a current measurement is completed
// @returns True on success, false otherwise
bool Axis::wait_for_current_meas() {
    return osSignalWait(M_SIGNAL_PH_CURRENT_MEAS, PH_CURRENT_MEAS_TIMEOUT).status == osEventSignal;
}

// step/direction interface
void Axis::step_cb() {
    if (step_dir_active_) {
        bool dir_pin = dir_gpio_->read();
        float dir = (dir_pin == GPIO_PIN_SET) ? 1.0f : -1.0f;
        controller_.pos_setpoint_ += dir * config_.counts_per_step;
    }
}

void Axis::decode_step_dir_pins() {
    if (step_gpio_)
        step_gpio_->deinit();
    if (dir_gpio_)
        dir_gpio_->deinit();
    if (config_.step_gpio_num < num_gpios)
        step_gpio_ = gpios[config_.step_gpio_num];
    else
        step_gpio_ = nullptr;
    if (config_.dir_gpio_num < num_gpios)
        dir_gpio_ = gpios[config_.dir_gpio_num];
    else
        dir_gpio_ = nullptr;
    // TODO: reinit GPIOs here
}

// @brief: Setup the watchdog reset value from the configuration watchdog timeout interval. 
void Axis::update_watchdog_settings() {

    if(config_.watchdog_timeout <= 0.0f) { // watchdog disabled 
        watchdog_reset_value_ = 0;
    } else if(config_.watchdog_timeout >= UINT32_MAX / (current_meas_hz+1)) { //overflow! 
        watchdog_reset_value_ = UINT32_MAX;
    } else {
        watchdog_reset_value_ = static_cast<uint32_t>(config_.watchdog_timeout * current_meas_hz);
    }

    // Do a feed to avoid instant timeout
    watchdog_feed();
}

// @brief (de)activates step/dir input
void Axis::set_step_dir_active(bool active) {
    if (active) {
        if (dir_gpio_) {
            dir_gpio_->init(GPIO_t::INPUT, GPIO_t::NO_PULL);
        }
        if (step_gpio_) {
            step_gpio_->init(GPIO_t::INPUT, GPIO_t::PULL_DOWN);
            step_gpio_->subscribe(true, false,
                [](void* ctx){ reinterpret_cast<Axis*>(ctx)->step_cb(); }, this);
        }

        step_dir_active_ = true;
    } else {
        step_dir_active_ = false;

        if (step_gpio_)
            step_gpio_->deinit();
        if (dir_gpio_)
            dir_gpio_->deinit();
    }
}

// @brief Do axis level checks and call subcomponent do_checks
// Returns true if everything is ok.
bool Axis::do_checks() {
    if (brake_resistor_enabled && !brake_resistor_armed)
        error_ |= ERROR_BRAKE_RESISTOR_DISARMED;
    if ((current_state_ != AXIS_STATE_IDLE) && !motor_.is_armed_)
        // motor got disarmed in something other than the idle loop
        error_ |= ERROR_MOTOR_DISARMED;
    if (!(vbus_voltage >= board_config.dc_bus_undervoltage_trip_level))
        error_ |= ERROR_DC_BUS_UNDER_VOLTAGE;
    if (!(vbus_voltage <= board_config.dc_bus_overvoltage_trip_level))
        error_ |= ERROR_DC_BUS_OVER_VOLTAGE;

    // Sub-components should use set_error which will propegate to this error_
    motor_.do_checks();
    encoder_.do_checks();
    // sensorless_estimator_.do_checks();
    // controller_.do_checks();

    return check_for_errors();
}

// @brief Update all esitmators
bool Axis::do_updates() {
    // Sub-components should use set_error which will propegate to this error_
    encoder_.update();
    sensorless_estimator_.update();
    return check_for_errors();
}

// @brief Feed the watchdog to prevent watchdog timeouts.
void Axis::watchdog_feed() {
    watchdog_current_value_ = watchdog_reset_value_;
}

// @brief Check the watchdog timer for expiration. Also sets the watchdog error bit if expired. 
bool Axis::watchdog_check() {
    // reset value = 0 means watchdog disabled. 
    if(watchdog_reset_value_ == 0) return true;

    // explicit check here to ensure that we don't underflow back to UINT32_MAX
    if(watchdog_current_value_ > 0) {
        watchdog_current_value_--;
        return true;
    } else {
        error_ |= ERROR_WATCHDOG_TIMER_EXPIRED;
        return false;
    }
}

bool Axis::run_lockin_spin() {
    // Spiral up current for softer rotor lock-in
    lockin_state_ = LOCKIN_STATE_RAMP;
    float x = 0.0f;
    run_control_loop([&]() {
        float phase = wrap_pm_pi(config_.lockin.ramp_distance * x);
        float I_mag = config_.lockin.current * x;
        x += current_meas_period / config_.lockin.ramp_time;
        if (!motor_.update(I_mag, phase, 0.0f))
            return false;
        return x < 1.0f;
    });
    
    // Spin states
    float distance = config_.lockin.ramp_distance;
    float phase = wrap_pm_pi(distance);
    float vel = distance / config_.lockin.ramp_time;

    // Function of states to check if we are done
    auto spin_done = [&](bool vel_override = false) -> bool {
        bool done = false;
        if (config_.lockin.finish_on_vel || vel_override)
            done = done || fabsf(vel) >= fabsf(config_.lockin.vel);
        if (config_.lockin.finish_on_distance)
            done = done || fabsf(distance) >= fabsf(config_.lockin.finish_distance);
        if (config_.lockin.finish_on_enc_idx)
            done = done || encoder_.index_found_;
        return done;
    };

    // Accelerate
    lockin_state_ = LOCKIN_STATE_ACCELERATE;
    run_control_loop([&]() {
        vel += config_.lockin.accel * current_meas_period;
        distance += vel * current_meas_period;
        phase = wrap_pm_pi(phase + vel * current_meas_period);

        if (!motor_.update(config_.lockin.current, phase, vel))
            return false;
        return !spin_done(true); //vel_override to go to next phase
    });

    if (!encoder_.index_found_)
        encoder_.set_idx_subscribe(true);

    // Constant speed
    if (!spin_done()) {
        lockin_state_ = LOCKIN_STATE_CONST_VEL;
        vel = config_.lockin.vel; // reset to actual specified vel to avoid small integration error
        run_control_loop([&]() {
            distance += vel * current_meas_period;
            phase = wrap_pm_pi(phase + vel * current_meas_period);

            if (!motor_.update(config_.lockin.current, phase, vel))
                return false;
            return !spin_done();
        });
    }

    lockin_state_ = LOCKIN_STATE_INACTIVE;
    return check_for_errors();
}

// Note run_sensorless_control_loop and run_closed_loop_control_loop are very similar and differ only in where we get the estimate from.
bool Axis::run_sensorless_control_loop() {
    run_control_loop([this](){
        if (controller_.config_.control_mode >= Controller::CTRL_MODE_POSITION_CONTROL)
            return error_ |= ERROR_POS_CTRL_DURING_SENSORLESS, false;

        // Note that all estimators are updated in the loop prefix in run_control_loop
        float current_setpoint;
        if (!controller_.update(sensorless_estimator_.pll_pos_, sensorless_estimator_.vel_estimate_, &current_setpoint))
            return error_ |= ERROR_CONTROLLER_FAILED, false;
        if (!motor_.update(current_setpoint, sensorless_estimator_.phase_, sensorless_estimator_.vel_estimate_))
            return false; // set_error should update axis.error_
        return true;
    });
    return check_for_errors();
}

bool Axis::run_closed_loop_control_loop() {
    // To avoid any transient on startup, we intialize the setpoint to be the current position
    controller_.pos_setpoint_ = encoder_.pos_estimate_;
    set_step_dir_active(config_.enable_step_dir);
    run_control_loop([this](){
        // Note that all estimators are updated in the loop prefix in run_control_loop
        float current_setpoint;
        if (!controller_.update(encoder_.pos_estimate_, encoder_.vel_estimate_, &current_setpoint))
            return error_ |= ERROR_CONTROLLER_FAILED, false; //TODO: Make controller.set_error
        float phase_vel = 2*M_PI * encoder_.vel_estimate_ / (float)encoder_.config_.cpr * motor_.config_.pole_pairs;
        if (!motor_.update(current_setpoint, encoder_.phase_, phase_vel))
            return false; // set_error should update axis.error_
        return true;
    });
    set_step_dir_active(false);
    return check_for_errors();
}

/**
 * @brief Spins the magnetic field at a fixed velocity (defined by the velocity
 * setpoint) and current/voltage setpoint. The current controller still runs in
 * closed loop mode.
 */
bool Axis::run_open_loop_control_loop() {
    set_step_dir_active(config_.enable_step_dir);

    run_control_loop([this](){
        float phase_vel;
        if (!motor_.config_.phase_locked) {
            phase_vel = 2 * M_PI * controller_.vel_setpoint_ * motor_.config_.pole_pairs;
            motor_.phase_setpoint_ = wrap_pm_pi(motor_.phase_setpoint_ + phase_vel * current_meas_period);
        } else {
            Axis other_axis = (this == &axes[0]) ? axes[1] : axes[0];
            if (other_axis.current_state_ != AXIS_STATE_OPEN_LOOP_CONTROL)
                return error_ |= ERROR_INVALID_STATE, false;
            phase_vel = 2 * M_PI * other_axis.controller_.vel_setpoint_ * other_axis.motor_.config_.pole_pairs;
            motor_.phase_setpoint_ = other_axis.motor_.phase_setpoint_; // TODO: add an offset here to account for delayed PWM
        }

        if (!motor_.update(controller_.current_setpoint_, motor_.phase_setpoint_, phase_vel))
            return false; // set_error should update axis.error_
        return true;
    });
    set_step_dir_active(false);
    return check_for_errors();
}

bool Axis::run_idle_loop() {
    // run_control_loop ignores missed modulation timing updates
    // if and only if we're in AXIS_STATE_IDLE
    safety_critical_disarm_motor_pwm(motor_);
    // the only valid reason to leave idle is an external request
    while (requested_state_ == AXIS_STATE_UNDEFINED) {
        run_control_loop([this](){
            return true;
        });
    }
    return check_for_errors();
}

// Infinite loop that does calibration and enters main control loop as appropriate
void Axis::run_state_machine_loop() {
    while (!thread_id_valid_) {
        // Wait until the main task has signalled the readiness of this task,
        // otherwise the current measurement updates won't signal this thread.
        osDelay(1);
    }

    // Allocate the map for anti-cogging algorithm and initialize all values to 0.0f
    // TODO: Move this somewhere else
    // TODO: respect changes of CPR
    int encoder_cpr = encoder_.config_.cpr;
    controller_.anticogging_.cogging_map = (float*)malloc(encoder_cpr * sizeof(float));
    if (controller_.anticogging_.cogging_map != NULL) {
        for (int i = 0; i < encoder_cpr; i++) {
            controller_.anticogging_.cogging_map[i] = 0.0f;
        }
    }

    // arm!
    motor_.arm();
    
    for (;;) {
        // Load the task chain if a specific request is pending
        if (requested_state_ != AXIS_STATE_UNDEFINED) {
            size_t pos = 0;
            if (requested_state_ == AXIS_STATE_STARTUP_SEQUENCE) {
                if (config_.startup_motor_calibration)
                    task_chain_[pos++] = AXIS_STATE_MOTOR_CALIBRATION;
                if (config_.startup_encoder_index_search && encoder_.config_.use_index)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_INDEX_SEARCH;
                if (config_.startup_encoder_offset_calibration)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_OFFSET_CALIBRATION;
                if (config_.startup_closed_loop_control)
                    task_chain_[pos++] = AXIS_STATE_CLOSED_LOOP_CONTROL;
                else if (config_.startup_sensorless_control)
                    task_chain_[pos++] = AXIS_STATE_SENSORLESS_CONTROL;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ == AXIS_STATE_FULL_CALIBRATION_SEQUENCE) {
                task_chain_[pos++] = AXIS_STATE_MOTOR_CALIBRATION;
                if (encoder_.config_.use_index)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_INDEX_SEARCH;
                task_chain_[pos++] = AXIS_STATE_ENCODER_OFFSET_CALIBRATION;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ != AXIS_STATE_UNDEFINED) {
                task_chain_[pos++] = requested_state_;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            }
            task_chain_[pos++] = AXIS_STATE_UNDEFINED; // TODO: bounds checking
            requested_state_ = AXIS_STATE_UNDEFINED;
            // Auto-clear any invalid state error
            error_ &= ~ERROR_INVALID_STATE;
        }

        // Note that current_state is a reference to task_chain_[0]

        // Run the specified state
        // Handlers should exit if requested_state != AXIS_STATE_UNDEFINED
        bool status;
        switch (current_state_) {
            case AXIS_STATE_PWM_TEST: {
                status = motor_.pwm_test(1.0f);
            } break;

            case AXIS_STATE_MOTOR_CALIBRATION: {
                status = motor_.run_calibration();
            } break;

            case AXIS_STATE_ENCODER_INDEX_SEARCH: {
                if (encoder_.config_.idx_search_unidirectional && motor_.config_.direction==0)
                    goto invalid_state_label;

                status = encoder_.run_index_search();
            } break;

            case AXIS_STATE_ENCODER_DIR_FIND: {
                status = encoder_.run_direction_find();
            } break;

            case AXIS_STATE_ENCODER_OFFSET_CALIBRATION: {
                status = encoder_.run_offset_calibration();
            } break;

            case AXIS_STATE_LOCKIN_SPIN: {
                if (motor_.config_.direction==0)
                    goto invalid_state_label;
                status = run_lockin_spin();
            } break;

            case AXIS_STATE_SENSORLESS_CONTROL: {
                if (motor_.config_.direction==0)
                        goto invalid_state_label;
                status = run_lockin_spin(); // TODO: restart if desired
                if (status) {
                    // call to controller.reset() that happend when arming means that vel_setpoint
                    // is zeroed. So we make the setpoint the spinup target for smooth transition.
                    controller_.vel_setpoint_ = config_.lockin.vel;
                    status = run_sensorless_control_loop();
                }
            } break;

            case AXIS_STATE_CLOSED_LOOP_CONTROL: {
                if (motor_.config_.direction==0)
                    goto invalid_state_label;
                if (!encoder_.is_ready_)
                    goto invalid_state_label;
                status = run_closed_loop_control_loop();
            } break;

            case AXIS_STATE_OPEN_LOOP_CONTROL: {
                if (motor_.config_.direction==0)
                    goto invalid_state_label;
                status = run_open_loop_control_loop();
            } break;

            case AXIS_STATE_IDLE: {
                run_idle_loop();
                status = motor_.arm(); // done with idling - try to arm the motor
            } break;

            default:
            invalid_state_label:
                error_ |= ERROR_INVALID_STATE;
                status = false; // this will set the state to idle
                break;
        }

        // If the state failed, go to idle, else advance task chain
        if (!status)
            current_state_ = AXIS_STATE_IDLE;
        else
            memmove(task_chain_, task_chain_ + 1, sizeof(task_chain_) - sizeof(task_chain_[0]));
    }
}
