#include "app/high_level.h"
#include <sys/syscall.h>
#include <sys/types.h>
#include "spdlog/spdlog.h"
#include "utils.h"

using namespace arx;

Arx5HighLevel::Arx5HighLevel(std::string model, std::string can_name,
                             std::string urdf_path)
    : _joint_controller(model, can_name), _solver(urdf_path) {
  _joint_controller.set_to_damping();
  // std::string model_path = std::string(ARX_DIRECTORY) + "/models/arx5.urdf";
  int control_mode = 0;  // Keyboard control_mode
  _high_state.pose_6d =
      _solver.forward_kinematics(_joint_controller.get_state().pos);
  _high_state.gripper_pos = _joint_controller.get_state().gripper_pos;
  _high_state.gripper_vel = _joint_controller.get_state().gripper_vel;
  _high_state.gripper_torque = _joint_controller.get_state().gripper_torque;
  _background_gravity_compensation =
      std::thread(&Arx5HighLevel::_background_gravity_compensation_task, this);
  std::cout << "Arx5HighLevel: Background send_recv task is running at ID:"
            << syscall(SYS_gettid) << std::endl;
  _background_gravity_compensation_running = true;

  // Init filters
  JointState low_state = _joint_controller.get_state();
  for (int i = 0; i < _moving_window_size; ++i) {
    _joint_pos_filter.filter(low_state.pos);
    // No need to init torque filter
  }
}

Arx5HighLevel::~Arx5HighLevel() {
  set_to_damping();
  sleep_ms(1000);
  _destroy_background_threads = true;
  _background_gravity_compensation.join();
  std::cout << "Arx5HighLevel: background gravity compensation task joined"
            << std::endl;
  _joint_controller.enable_background_send_recv();
  spdlog::info("Arx5HighLevel: Enabled low level communication");
}

void Arx5HighLevel::set_high_cmd(HighState new_cmd) {
  std::lock_guard<std::mutex> guard_cmd(_cmd_mutex);
  if (new_cmd.gripper_vel != 0 || new_cmd.gripper_torque != 0) {
    spdlog::warn(
        "Arx5HighLevel: Gripper velocity and torque control is not supported "
        "yet. Will be ignored");
    new_cmd.gripper_vel = 0;
    new_cmd.gripper_torque = 0;
  }
  if (new_cmd.timestamp == 0) {
    double t = get_timestamp();
    new_cmd.timestamp = t + _LOOK_AHEAD_TIME;
  } else {
    // TODO: support scheduling future targets
    spdlog::warn("Arx5HighLevel: Timestamp is set to {:.3f}, will be ignored",
                 new_cmd.timestamp);
    double t = get_timestamp();
    new_cmd.timestamp = t + _LOOK_AHEAD_TIME;
  }
  _input_high_cmd = new_cmd;
}

std::tuple<HighState, HighState> Arx5HighLevel::get_high_cmd() {
  std::lock_guard<std::mutex> guard_cmd(_cmd_mutex);
  return std::make_tuple(_input_high_cmd, _output_high_cmd);
}

std::tuple<JointState, JointState> Arx5HighLevel::get_joint_cmd() {
  return _joint_controller.get_joint_cmd();
}

HighState Arx5HighLevel::get_high_state() {
  std::lock_guard<std::mutex> guard_state(_state_mutex);
  return _high_state;
}

JointState Arx5HighLevel::get_joint_state() {
  return _joint_state;
}

double Arx5HighLevel::get_timestamp() {
  return _joint_state.timestamp;
}

void Arx5HighLevel::reset_to_home() {

  JointState low_cmd;
  HighState high_cmd;
  Gain gain;
  JointState init_state = get_joint_state();
  Gain init_gain = get_gain();
  Gain target_gain =
      Gain(DEFAULT_KP, DEFAULT_KD, DEFAULT_GRIPPER_KP, DEFAULT_GRIPPER_KD);
  JointState target_state;
  target_state.gripper_pos = GRIPPER_WIDTH;

  // calculate the maximum joint position error
  double max_pos_error = (init_state.pos - Vec6d::Zero()).cwiseAbs().maxCoeff();
  max_pos_error =
      std::max(max_pos_error,
               (GRIPPER_WIDTH - init_state.gripper_pos) * 2 / GRIPPER_WIDTH);
  // interpolate from current kp kd to default kp kd in max(max_pos_error*2, 0.5)s
  // and keep the target for 0.5s
  double step_num = std::max(max_pos_error * 2, 0.5) / HIGH_LEVEL_DT;
  std::cout << "Arx5HighLevel: Start reset to home in "
            << std::max(max_pos_error, double(0.5)) + 0.5
            << " s, max_pos_error:" << max_pos_error << std::endl;

  for (int i = 0; i <= step_num; ++i) {
    double alpha = double(i) / step_num;
    gain = init_gain * (1 - alpha) + target_gain * alpha;
    low_cmd = init_state * (1 - alpha) + target_state * alpha;
    set_gain(gain);
    high_cmd.pose_6d = _solver.forward_kinematics(low_cmd.pos);
    high_cmd.gripper_pos = low_cmd.gripper_pos;
    set_high_cmd(high_cmd);
    sleep_ms(5);
  }
  sleep_ms(500);
  std::cout << "Arx5HighLevel: Finish reset to home" << std::endl;
}

void Arx5HighLevel::set_to_damping() {
  JointState low_cmd;
  HighState high_cmd;
  JointState low_state;
  Gain gain;
  JointState init_state = get_joint_state();
  Gain init_gain = get_gain();
  Gain target_gain;
  target_gain.kd = DEFAULT_KD;
  std::cout << "Arx5HighLevel: Start set to damping" << std::endl;

  low_state = get_joint_state();
  high_cmd.pose_6d = _solver.forward_kinematics(low_state.pos);
  high_cmd.gripper_pos = low_state.gripper_pos;
  set_gain(target_gain);
  set_high_cmd(high_cmd);

  sleep_ms(500);
  std::cout << "Arx5HighLevel: Finish set to damping" << std::endl;
}

void Arx5HighLevel::set_gain(Gain new_gain) {
  // std::cout << "Arx5HighLevel: Set gain to kp: " << new_gain.kp.transpose() << " kd: " << new_gain.kd.transpose() << std::endl;
  _joint_controller.set_gain(new_gain);
}

Gain Arx5HighLevel::get_gain() {
  return _joint_controller.get_gain();
}
void Arx5HighLevel::_update_output_cmd() {
  HighState prev_output_high_cmd = _output_high_cmd;
  // Interpolate the output command according to look ahead time
  double t = get_timestamp();
  if (t + HIGH_LEVEL_DT >= _input_high_cmd.timestamp) {
    _output_high_cmd = _input_high_cmd;
  } else {
    // double alpha = (_input_high_cmd.timestamp - t - HIGH_LEVEL_DT) / _LOOK_AHEAD_TIME;
    // //spdlog::debug("Arx5HighLevel: alpha: %.3f", alpha);

    // double alpha = 0.95;
    // _output_high_cmd = _input_high_cmd * (1 - alpha) + prev_output_high_cmd * alpha;
    _output_high_cmd = _input_high_cmd;
  }

  if (_enable_ee_vel_clipping) {
    double dt = HIGH_LEVEL_DT;
    Vec6d prev_ee_pose = prev_output_high_cmd.pose_6d;
    Gain gain = get_gain();
    for (int i = 0; i < 6; ++i) {
      if (gain.kp[i] > 0) {
        double max_ee_vel = EE_VEL_MAX[i];
        double max_ee_pose = prev_ee_pose[i] + max_ee_vel * dt;
        double min_ee_pose = prev_ee_pose[i] - max_ee_vel * dt;
        if (_output_high_cmd.pose_6d[i] > max_ee_pose) {
          if (_output_high_cmd.pose_6d[i] >
              max_ee_pose + _clipping_output_threshold) {
            spdlog::debug(
                "Arx5HighLevel: Clipping {} from {:.3f} to {:.3f} (current "
                "{:.3f})",
                EE_POSE_NAMES[i].c_str(), _output_high_cmd.pose_6d[i],
                max_ee_pose, prev_ee_pose[i]);
          }
          _output_high_cmd.pose_6d[i] = max_ee_pose;
        } else if (_output_high_cmd.pose_6d[i] < min_ee_pose) {
          if (_output_high_cmd.pose_6d[i] <
              min_ee_pose - _clipping_output_threshold) {
            spdlog::debug(
                "Arx5HighLevel: Clipping %s from {:.3f} to {:.3f} (current "
                "{:.3f})",
                EE_POSE_NAMES[i].c_str(), _output_high_cmd.pose_6d[i],
                min_ee_pose, prev_ee_pose[i]);
          }
          _output_high_cmd.pose_6d[i] = min_ee_pose;
        }
      } else {
        _output_high_cmd.pose_6d[i] = _high_state.pose_6d[i];
      }
    }
  }
}
void Arx5HighLevel::_background_gravity_compensation_task() {
  while (!_destroy_background_threads) {
    int start_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (_background_gravity_compensation_running) {

      JointState low_cmd;
      JointState low_state = _joint_controller.get_state();
      std::tuple<bool, Vec6d> ik_results;
      {
        std::lock_guard<std::mutex> guard_cmd(_cmd_mutex);
        _update_output_cmd();
        ik_results =
            _solver.inverse_kinematics(_output_high_cmd.pose_6d, low_state.pos);
        low_cmd.gripper_pos = _output_high_cmd.gripper_pos;
      }
      bool success = std::get<0>(ik_results);
      Vec6d joint_pos = std::get<1>(ik_results);

      if (success) {
        low_cmd.pos = _joint_pos_filter.filter(joint_pos);
        // Use the torque of the current joint positions
        Vec6d joint_torque =
            _solver.inverse_dynamics(low_state.pos, Vec6d(), Vec6d());
        low_cmd.torque = _joint_torque_filter.filter(joint_torque);
        _joint_controller.set_joint_cmd(low_cmd);
        // printf("Arx5HighLevel: Joint positions: %.3f %.3f %.3f %.3f %.3f %.3f, cmd: %.3f %.3f %.3f %.3f %.3f %.3f, Torque: %.3f %.3f %.3f %.3f %.3f %.3f",
        //        low_state.pos[0], low_state.pos[1], low_state.pos[2], low_state.pos[3], low_state.pos[4], low_state.pos[5],
        //        joint_pos[0], joint_pos[1], joint_pos[2], joint_pos[3], joint_pos[4], joint_pos[5],
        //        low_cmd.torque[0], low_cmd.torque[1], low_cmd.torque[2], low_cmd.torque[3], low_cmd.torque[4], low_cmd.torque[5]);
      }
      _joint_controller.send_recv_once();
      _joint_state = _joint_controller.get_state();
      _high_state.pose_6d = _solver.forward_kinematics(_joint_state.pos);
      _high_state.gripper_pos = _joint_state.gripper_pos;
      _high_state.gripper_vel = _joint_state.gripper_vel;
      _high_state.gripper_torque = _joint_state.gripper_torque;
      _high_state.timestamp = _joint_state.timestamp;
    }
    int solve_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count() -
                        start_time_us;
    // Usually takes 3ms
    if (solve_time_us < HIGH_LEVEL_DT * 1e6)
      std::this_thread::sleep_for(
          std::chrono::microseconds(int(HIGH_LEVEL_DT * 1e6) - solve_time_us));
    else {
      spdlog::warn(
          "Arx5HighLevel: Background gravity compensation task takes {:.3f} ms",
          solve_time_us / 1000.0);
    }
  }
}
