#include "app/cartesian_controller.h"
#include <sys/syscall.h>
#include <sys/types.h>
#include "spdlog/spdlog.h"
#include "utils.h"

namespace arx {

Arx5CartesianController::Arx5CartesianController(std::string model,
                                                 std::string can_name,
                                                 std::string urdf_path)
    : _can_handle(can_name),
      _logger(spdlog::stdout_color_mt(model + std::string("_") + can_name)),
      _ROBOT_CONFIG(RobotConfig(model, _CONTROLLER_DT)),
      _solver(std::make_shared<Arx5Solver>(urdf_path)) {
  _logger->set_pattern("[%H:%M:%S %n %^%l%$] %v");

  _init_robot();
  _background_send_recv_thread =
      std::thread(&Arx5CartesianController::_background_send_recv, this);
  _logger->info("Background send_recv task is running at ID: {}",
                syscall(SYS_gettid));
}

Arx5CartesianController::~Arx5CartesianController() {
  Gain damping_gain = Gain();
  damping_gain.kd = _ROBOT_CONFIG.default_kd;
  damping_gain.kd[0] *= 3;
  damping_gain.kd[1] *= 3;
  damping_gain.kd[2] *= 3;
  damping_gain.kd[3] *= 1.5;
  _logger->info("Set to damping before exit");
  set_gain(damping_gain);
  _input_joint_cmd.vel = Vec6d::Zero();
  _input_joint_cmd.torque = Vec6d::Zero();
  _enable_gravity_compensation = false;
  sleep_ms(2000);
  _destroy_background_threads = true;
  _background_send_recv_thread.join();
  _logger->info("background send_recv task joined");
}

void Arx5CartesianController::_init_robot() {

  bool succeeded = true;
  for (int i = 0; i < 7; ++i) {
    if (_ROBOT_CONFIG.motor_type[i] == MotorType::DM_J4310 ||
        _ROBOT_CONFIG.motor_type[i] == MotorType::DM_J4340) {
      int id = _ROBOT_CONFIG.motor_id[i];
      succeeded = _can_handle.enable_DM_motor(id);
      if (!succeeded) {
        throw std::runtime_error("Failed to enable motor " +
                                 std::to_string(id) +
                                 ". Please check the connection.");
      }
      usleep(1000);
    }
  }

  Gain gain = Gain();
  gain.kd = _ROBOT_CONFIG.default_kd;
  _input_joint_cmd = JointState();  // initialize joint command to zero
  set_gain(gain);                   // set to damping by default
  for (int i = 0; i <= 10; ++i) {
    // make sure all the motor positions are updated
    succeeded = _send_recv();
    if (!succeeded) {
      throw std::runtime_error("Failed to send and receive motor command.");
    }
    sleep_ms(5);
  }
  // Check whether any motor has non-zero position
  if (_joint_state.pos == Vec6d::Zero()) {
    _logger->error(
        "All motors are not initialized. Please check the "
        "connection or power of the arm.");
    throw std::runtime_error(
        "All motors are not initialized. Please check the "
        "connection or power of the arm.");
  }
  _background_send_recv_running = true;
}

void Arx5CartesianController::set_eef_cmd(EEFState new_cmd) {
  std::lock_guard<std::mutex> lock(_cmd_mutex);
  if (new_cmd.gripper_vel != 0 || new_cmd.gripper_torque != 0) {
    _logger->warn("Gripper vel and torque control is not supported yet.");
    new_cmd.gripper_vel = 0;
    new_cmd.gripper_torque = 0;
  }
  _input_eef_cmd = new_cmd;
  // TODO: output eef cmd filtering
  _output_eef_cmd = new_cmd;
}

std::tuple<EEFState, EEFState> Arx5CartesianController::get_eef_cmd() {
  std::lock_guard<std::mutex> lock(_cmd_mutex);
  return std::make_tuple(_input_eef_cmd, _output_eef_cmd);
}

EEFState Arx5CartesianController::get_eef_state() {
  std::lock_guard<std::mutex> lock(_state_mutex);
  EEFState eef_state;
  eef_state.pose_6d = _solver->forward_kinematics(_joint_state.pos);
  eef_state.gripper_pos = _joint_state.gripper_pos;
  eef_state.gripper_vel = _joint_state.gripper_vel;
  eef_state.gripper_torque = _joint_state.gripper_torque;
  return eef_state;
}

JointState Arx5CartesianController::get_joint_state() {
  std::lock_guard<std::mutex> lock(_state_mutex);
  return _joint_state;
}

std::tuple<JointState, JointState> Arx5CartesianController::get_joint_cmd() {
  std::lock_guard<std::mutex> lock(_cmd_mutex);
  return std::make_tuple(_input_joint_cmd, _output_joint_cmd);
}

double Arx5CartesianController::get_timestamp() {
  return double(get_time_us() - _start_time_us) / 1e6;
}

Gain Arx5CartesianController::get_gain() {
  std::lock_guard<std::mutex> lock(_cmd_mutex);
  return _gain;
}

void Arx5CartesianController::set_gain(Gain new_gain) {

  // Make sure the robot doesn't jump when setting kp to non-zero
  if (_gain.kp.isZero() && !new_gain.kp.isZero()) {
    double max_pos_error =
        (_joint_state.pos - _output_joint_cmd.pos).cwiseAbs().maxCoeff();
    double error_threshold = 0.2;
    if (max_pos_error > error_threshold) {
      _logger->error(
          "Cannot set kp to non-zero when the joint pos cmd is far from "
          "current pos.");
      _logger->error("Current pos: {}, cmd pos: {}, threshold: {}",
                     vec2str(_joint_state.pos), vec2str(_output_joint_cmd.pos),
                     error_threshold);
      _background_send_recv_running = false;
      throw std::runtime_error(
          "Cannot set kp to non-zero when the joint pos cmd is far from "
          "current pos.");
    }
  }
  _gain = new_gain;
}

RobotConfig Arx5CartesianController::get_robot_config() {
  return _ROBOT_CONFIG;
}

void Arx5CartesianController::reset_to_home() {
  JointState joint_cmd;
  EEFState eef_cmd;
  Gain gain;
  JointState init_state = get_joint_state();
  Gain init_gain = get_gain();
  Gain target_gain =
      Gain(_ROBOT_CONFIG.default_kp, _ROBOT_CONFIG.default_kd,
           _ROBOT_CONFIG.default_gripper_kp, _ROBOT_CONFIG.default_gripper_kd);
  JointState target_state;
  target_state.gripper_pos = _ROBOT_CONFIG.gripper_width;

  // calculate the maximum joint position error
  double max_pos_error = (init_state.pos - Vec6d::Zero()).cwiseAbs().maxCoeff();
  max_pos_error = std::max(
      max_pos_error, init_state.gripper_pos * 2 / _ROBOT_CONFIG.gripper_width);
  // interpolate from current kp kd to default kp kd in max(max_pos_error*2, 0.5)s
  // and keep the target for 0.5s
  double step_num =
      std::max(max_pos_error * 2, 0.5) / _ROBOT_CONFIG.controller_dt;
  _logger->info("Start reset to home in {:.3f}s, max_pos_error: {:.3f}",
                std::max(max_pos_error * 2, double(0.5)) + 0.5, max_pos_error);

  bool prev_running = _background_send_recv_running;
  _background_send_recv_running = true;
  for (int i = 0; i <= step_num; ++i) {
    double alpha = double(i) / step_num;
    gain = init_gain * (1 - alpha) + target_gain * alpha;
    joint_cmd = init_state * (1 - alpha) + target_state * alpha;
    set_gain(gain);
    eef_cmd.pose_6d = _solver->forward_kinematics(joint_cmd.pos);
    eef_cmd.gripper_pos = joint_cmd.gripper_pos;
    set_eef_cmd(eef_cmd);
    sleep_ms(5);
  }
  sleep_ms(500);
  _logger->info("Finish reset to home");
  _background_send_recv_running = prev_running;
}

void Arx5CartesianController::set_to_damping() {
  JointState joint_cmd;
  EEFState eef_cmd;
  JointState joint_state;
  Gain gain;
  JointState init_state = get_joint_state();
  Gain init_gain = get_gain();
  Gain target_gain;
  target_gain.kd = _ROBOT_CONFIG.default_kd;
  _logger->info("Start set to damping");

  joint_state = get_joint_state();
  eef_cmd.pose_6d = _solver->forward_kinematics(joint_state.pos);
  eef_cmd.gripper_pos = joint_state.gripper_pos;
  set_gain(target_gain);
  set_eef_cmd(eef_cmd);

  sleep_ms(500);
  _logger->info("Finish set to damping");
}

void Arx5CartesianController::set_log_level(
    spdlog::level::level_enum log_level) {
  _logger->set_level(log_level);
}

void Arx5CartesianController::_update_output_cmd() {
  std::lock_guard<std::mutex> guard_cmd(_cmd_mutex);

  JointState prev_output_cmd = _output_joint_cmd;

  _output_joint_cmd = _input_joint_cmd;

  // Joint velocity clipping
  double dt = _CONTROLLER_DT;
  for (int i = 0; i < 6; ++i) {
    if (_gain.kp[i] > 0) {
      double delta_pos = _input_joint_cmd.pos[i] - prev_output_cmd.pos[i];
      double max_vel = _ROBOT_CONFIG.joint_vel_max[i];
      if (std::abs(delta_pos) > max_vel * dt) {
        _output_joint_cmd.pos[i] =
            prev_output_cmd.pos[i] +
            max_vel * dt * delta_pos / std::abs(delta_pos);
        _logger->debug("Joint {} pos {:.3f} pos cmd clipped: {:.3f} to {:.3f}",
                       i, _joint_state.pos[i], _input_joint_cmd.pos[i],
                       _output_joint_cmd.pos[i]);
      }
    } else {
      _output_joint_cmd.pos[i] = _joint_state.pos[i];
    }
    if (_gain.gripper_kp > 0) {
      double gripper_delta_pos =
          _input_joint_cmd.gripper_pos - prev_output_cmd.gripper_pos;
      if (std::abs(gripper_delta_pos) / dt > _ROBOT_CONFIG.gripper_vel_max) {
        _output_joint_cmd.gripper_pos =
            prev_output_cmd.gripper_pos + _ROBOT_CONFIG.gripper_vel_max * dt *
                                              gripper_delta_pos /
                                              std::abs(gripper_delta_pos);
        _logger->debug("Gripper pos cmd clipped: {:.3f} to {:.3f}",
                       _input_joint_cmd.gripper_pos,
                       _output_joint_cmd.gripper_pos);
      }
    } else {
      _output_joint_cmd.gripper_pos = _joint_state.gripper_pos;
    }
  }

  // Joint pos clipping
  for (int i = 0; i < 6; ++i) {
    if (_output_joint_cmd.pos[i] < _ROBOT_CONFIG.joint_pos_min[i]) {
      _logger->debug(
          "Joint {} pos {:.3f} pos cmd clipped from {:.3f} to min {:.3f}", i,
          _joint_state.pos[i], _output_joint_cmd.pos[i],
          _ROBOT_CONFIG.joint_pos_min[i]);
      _output_joint_cmd.pos[i] = _ROBOT_CONFIG.joint_pos_min[i];
    } else if (_output_joint_cmd.pos[i] > _ROBOT_CONFIG.joint_pos_max[i]) {
      _logger->debug(
          "Joint {} pos {:.3f} pos cmd clipped from {:.3f} to max {:.3f}", i,
          _joint_state.pos[i], _output_joint_cmd.pos[i],
          _ROBOT_CONFIG.joint_pos_max[i]);
      _output_joint_cmd.pos[i] = _ROBOT_CONFIG.joint_pos_max[i];
    }
  }
  // Gripper pos clipping
  if (_output_joint_cmd.gripper_pos < 0) {
    if (_output_joint_cmd.gripper_pos < -0.005)
      _logger->debug("Gripper pos cmd clipped from {:.3f} to min: {:.3f}",
                     _output_joint_cmd.gripper_pos, 0.0);
    _output_joint_cmd.gripper_pos = 0;
  } else if (_output_joint_cmd.gripper_pos > _ROBOT_CONFIG.gripper_width) {
    if (_output_joint_cmd.gripper_pos > _ROBOT_CONFIG.gripper_width + 0.005)
      _logger->debug("Gripper pos cmd clipped from {:.3f} to max: {:.3f}",
                     _output_joint_cmd.gripper_pos,
                     _ROBOT_CONFIG.gripper_width);
    _output_joint_cmd.gripper_pos = _ROBOT_CONFIG.gripper_width;
  }
  if (std::abs(_joint_state.gripper_torque) >
      _ROBOT_CONFIG.gripper_torque_max / 2) {
    double sign = _joint_state.gripper_torque > 0
                      ? 1
                      : -1;  // -1 for closing blocked, 1 for opening blocked
    double delta_pos =
        _output_joint_cmd.gripper_pos -
        prev_output_cmd
            .gripper_pos;  // negative for closing, positive for opening
    if (delta_pos * sign > 0) {
      _logger->debug(
          "Gripper torque is too large, gripper pos cmd is not updated");
      _output_joint_cmd.gripper_pos = prev_output_cmd.gripper_pos;
    }
  }

  // Torque clipping
  for (int i = 0; i < 6; ++i) {
    if (_output_joint_cmd.torque[i] > _ROBOT_CONFIG.joint_torque_max[i]) {
      _logger->debug("Joint {} torque cmd clipped from {:.3f} to max {:.3f}", i,
                     _output_joint_cmd.torque[i],
                     _ROBOT_CONFIG.joint_torque_max[i]);
      _output_joint_cmd.torque[i] = _ROBOT_CONFIG.joint_torque_max[i];
    } else if (_output_joint_cmd.torque[i] <
               -_ROBOT_CONFIG.joint_torque_max[i]) {
      _logger->debug("Joint {} torque cmd clipped from {:.3f} to min {:.3f}", i,
                     _output_joint_cmd.torque[i],
                     -_ROBOT_CONFIG.joint_torque_max[i]);
      _output_joint_cmd.torque[i] = -_ROBOT_CONFIG.joint_torque_max[i];
    }
  }
}

void Arx5CartesianController::_over_current_protection() {
  bool over_current = false;
  for (int i = 0; i < 6; ++i) {
    if (std::abs(_joint_state.torque[i]) > _ROBOT_CONFIG.joint_torque_max[i]) {
      over_current = true;
      _logger->error("Over current detected once on joint {}, current: {:.3f}",
                     i, _joint_state.torque[i]);
      break;
    }
  }
  if (std::abs(_joint_state.gripper_torque) >
      _ROBOT_CONFIG.gripper_torque_max) {
    over_current = true;
    _logger->error("Over current detected once on gripper, current: {:.3f}",
                   _joint_state.gripper_torque);
  }
  if (over_current) {
    _over_current_cnt++;
    if (_over_current_cnt > _ROBOT_CONFIG.over_current_cnt_max) {
      _logger->error(
          "Over current detected, robot is set to damping. Please "
          "restart the "
          "program.");
      _enter_emergency_state();
    }
  } else {
    _over_current_cnt = 0;
  }
}

void Arx5CartesianController::_check_joint_state_sanity() {
  for (int i = 0; i < 6; ++i) {
    if (std::abs(_joint_state.pos[i]) > _ROBOT_CONFIG.joint_pos_max[i] + 3.14 ||
        std::abs(_joint_state.pos[i]) < _ROBOT_CONFIG.joint_pos_min[i] - 3.14) {
      _logger->error(
          "Joint {} pos data error: {:.3f}. Please restart the program.", i,
          _joint_state.pos[i]);
      _enter_emergency_state();
    }
    if (std::abs(_input_joint_cmd.pos[i]) >
            _ROBOT_CONFIG.joint_pos_max[i] + 3.14 ||
        std::abs(_input_joint_cmd.pos[i]) <
            _ROBOT_CONFIG.joint_pos_min[i] - 3.14) {
      _logger->error(
          "Joint {} command data error: {:.3f}. Please restart the "
          "program.",
          i, _input_joint_cmd.pos[i]);
      _enter_emergency_state();
    }
    if (std::abs(_joint_state.torque[i]) >
        100 * _ROBOT_CONFIG.joint_torque_max[i]) {
      _logger->error(
          "Joint {} torque data error: {:.3f}. Please restart the "
          "program.",
          i, _joint_state.torque[i]);
      _enter_emergency_state();
    }
  }
}

void Arx5CartesianController::_enter_emergency_state() {
  Gain damping_gain;
  damping_gain.kd = _ROBOT_CONFIG.default_kd;
  damping_gain.kd[1] *= 3;
  damping_gain.kd[2] *= 3;
  damping_gain.kd[3] *= 1.5;
  set_gain(damping_gain);
  _input_joint_cmd.vel = Vec6d::Zero();
  _input_joint_cmd.torque = Vec6d::Zero();

  while (true) {
    _send_recv();
    sleep_ms(5);
  }
}

bool Arx5CartesianController::_send_recv() {
  // TODO: in the motor documentation, there shouldn't be these torque constants. Torque will go directly into the motors
  const double torque_constant_EC_A4310 = 1.4;  // Nm/A
  const double torque_constant_DM_J4310 = 0.424;
  const double torque_constant_DM_J4340 = 1.0;
  bool succeeded = true;
  int start_time_us = get_time_us();

  _update_output_cmd();
  int update_cmd_time_us = get_time_us();
  int communicate_sleep_us = 150;

  for (int i = 0; i < 6; i++) {
    int start_send_motor_time_us = get_time_us();
    if (_ROBOT_CONFIG.motor_type[i] == MotorType::EC_A4310) {
      succeeded = _can_handle.send_EC_motor_cmd(
          _ROBOT_CONFIG.motor_id[i], _gain.kp[i], _gain.kd[i],
          _output_joint_cmd.pos[i], _output_joint_cmd.vel[i],
          _output_joint_cmd.torque[i] / torque_constant_EC_A4310);
    } else if (_ROBOT_CONFIG.motor_type[i] == MotorType::DM_J4310) {

      succeeded = _can_handle.send_DM_motor_cmd(
          _ROBOT_CONFIG.motor_id[i], _gain.kp[i], _gain.kd[i],
          _output_joint_cmd.pos[i], _output_joint_cmd.vel[i],
          _output_joint_cmd.torque[i] / torque_constant_DM_J4310);

    } else if (_ROBOT_CONFIG.motor_type[i] == MotorType::DM_J4340) {
      succeeded = _can_handle.send_DM_motor_cmd(
          _ROBOT_CONFIG.motor_id[i], _gain.kp[i], _gain.kd[i],
          _output_joint_cmd.pos[i], _output_joint_cmd.vel[i],
          _output_joint_cmd.torque[i] / torque_constant_DM_J4340);
    } else {
      _logger->error("Motor type not supported.");
      return false;
    }
    int finish_send_motor_time_us = get_time_us();
    if (!succeeded) {
      _logger->error("Failed to send motor command to motor {}",
                     _ROBOT_CONFIG.motor_id[i]);
      return false;
    }
    sleep_us(communicate_sleep_us -
             (finish_send_motor_time_us - start_send_motor_time_us));
  }

  // Send gripper command (gripper is using DM motor)
  int start_send_motor_time_us = get_time_us();

  double gripper_motor_pos = _output_joint_cmd.gripper_pos /
                             _ROBOT_CONFIG.gripper_width *
                             _ROBOT_CONFIG.gripper_open_readout;
  succeeded =
      _can_handle.send_DM_motor_cmd(_ROBOT_CONFIG.motor_id[6], _gain.gripper_kp,
                                    _gain.gripper_kd, gripper_motor_pos, 0, 0);
  int finish_send_motor_time_us = get_time_us();

  if (!succeeded) {
    _logger->error("Failed to send motor command to motor {}",
                   _ROBOT_CONFIG.motor_id[6]);
    return false;
  }
  sleep_us(communicate_sleep_us -
           (finish_send_motor_time_us - start_send_motor_time_us));

  int start_get_motor_msg_time_us = get_time_us();
  std::array<OD_Motor_Msg, 10> motor_msg = _can_handle.get_motor_msg();
  int get_motor_msg_time_us = get_time_us();

  // _logger->trace("update_cmd: {} us, send_motor_0: {} us, send_motor_1: {} us, send_motor_2: {} us, send_motor_3: {} us, send_motor_4: {} us, send_motor_5: {} us, send_motor_6: {} us, get_motor_msg: {} us",
  //                update_cmd_time_us - start_time_us, send_motor_0_time_us - start_send_motor_0_time_us, send_motor_1_time_us - start_send_motor_1_time_us, send_motor_2_time_us - start_send_motor_2_time_us,
  //                send_motor_3_time_us - start_send_motor_3_time_us, send_motor_4_time_us - start_send_motor_4_time_us, send_motor_5_time_us - start_send_motor_5_time_us, send_motor_6_time_us - start_send_motor_6_time_us, get_motor_msg_time_us - start_get_motor_msg_time_us);

  std::lock_guard<std::mutex> guard_state(_state_mutex);

  _joint_state.pos[0] = motor_msg[0].angle_actual_rad;
  _joint_state.pos[1] = motor_msg[1].angle_actual_rad;
  _joint_state.pos[2] = motor_msg[3].angle_actual_rad;
  _joint_state.pos[3] = motor_msg[4].angle_actual_rad;
  _joint_state.pos[4] = motor_msg[5].angle_actual_rad;
  _joint_state.pos[5] = motor_msg[6].angle_actual_rad;
  _joint_state.gripper_pos = motor_msg[7].angle_actual_rad /
                             _ROBOT_CONFIG.gripper_open_readout *
                             _ROBOT_CONFIG.gripper_width;

  _joint_state.vel[0] = motor_msg[0].speed_actual_rad;
  _joint_state.vel[1] = motor_msg[1].speed_actual_rad;
  _joint_state.vel[2] = motor_msg[3].speed_actual_rad;
  _joint_state.vel[3] = motor_msg[4].speed_actual_rad;
  _joint_state.vel[4] = motor_msg[5].speed_actual_rad;
  _joint_state.vel[5] = motor_msg[6].speed_actual_rad;
  _joint_state.gripper_vel = motor_msg[7].speed_actual_rad /
                             _ROBOT_CONFIG.gripper_open_readout *
                             _ROBOT_CONFIG.gripper_width;

  // HACK: just to match the values (there must be something wrong)
  for (int i = 0; i < 6; i++) {
    if (_ROBOT_CONFIG.motor_type[i] == MotorType::EC_A4310) {
      _joint_state.torque[i] = motor_msg[i].current_actual_float *
                               torque_constant_EC_A4310 *
                               torque_constant_EC_A4310;
      // Why there are two torque_constant_EC_A4310?
    } else if (_ROBOT_CONFIG.motor_type[i] == MotorType::DM_J4310) {
      _joint_state.torque[i] =
          motor_msg[i].current_actual_float * torque_constant_DM_J4310;
    } else if (_ROBOT_CONFIG.motor_type[i] == MotorType::DM_J4340) {
      _joint_state.torque[i] =
          motor_msg[i].current_actual_float * torque_constant_DM_J4340;
    }
  }
  _joint_state.gripper_torque =
      motor_msg[7].current_actual_float * torque_constant_DM_J4310;
  _joint_state.timestamp = get_timestamp();
  return true;
}

void Arx5CartesianController::_calc_joint_cmd() {
  JointState joint_cmd;
  JointState joint_state = get_joint_state();
  std::tuple<bool, Vec6d> ik_results;
  {
    std::lock_guard<std::mutex> guard_cmd(_cmd_mutex);
    ik_results =
        _solver->inverse_kinematics(_output_eef_cmd.pose_6d, joint_state.pos);
    joint_cmd.gripper_pos = _output_eef_cmd.gripper_pos;
  }
  bool success = std::get<0>(ik_results);
  Vec6d joint_pos = std::get<1>(ik_results);

  if (success) {
    joint_cmd.pos = _joint_pos_filter.filter(joint_pos);
    if (_enable_gravity_compensation) {
      // Use the torque of the current joint positions
      Vec6d joint_torque = _solver->inverse_dynamics(
          _joint_state.pos, Vec6d::Zero(), Vec6d::Zero());
      joint_cmd.torque = _joint_torque_filter.filter(joint_torque);
    }
    _input_joint_cmd = joint_cmd;
  }
}

void Arx5CartesianController::_background_send_recv() {
  while (!_destroy_background_threads) {
    int start_time_us = get_time_us();
    if (_background_send_recv_running) {
      _over_current_protection();
      _check_joint_state_sanity();
      _calc_joint_cmd();
      _send_recv();
    }
    int elapsed_time_us = get_time_us() - start_time_us;
    int sleep_time_us =
        int(_ROBOT_CONFIG.controller_dt * 1e6) - elapsed_time_us;
    if (sleep_time_us > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(sleep_time_us));
    } else if (sleep_time_us < -500) {
      _logger->debug(
          "Background send_recv task is running too slow, time: {} us",
          elapsed_time_us);
    }
  }
}
}  // namespace arx