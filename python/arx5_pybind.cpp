#include <pybind11/eigen.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "app/cartesian_controller.h"
#include "app/common.h"
#include "app/joint_controller.h"
#include "spdlog/spdlog.h"
#include "utils.h"
namespace py = pybind11;
using namespace arx;
using Vec6d = Eigen::Matrix<double, 6, 1>;
PYBIND11_MODULE(arx5_interface, m) {
  py::enum_<spdlog::level::level_enum>(m, "LogLevel")
      .value("TRACE", spdlog::level::level_enum::trace)
      .value("DEBUG", spdlog::level::level_enum::debug)
      .value("INFO", spdlog::level::level_enum::info)
      .value("WARNING", spdlog::level::level_enum::warn)
      .value("ERROR", spdlog::level::level_enum::err)
      .value("CRITICAL", spdlog::level::level_enum::critical)
      .value("OFF", spdlog::level::level_enum::off)
      .export_values();
  py::class_<JointState>(m, "JointState")
      .def(py::init<>())
      .def(py::init<Vec6d, Vec6d, Vec6d, double>())
      .def_readwrite("timestamp", &JointState::timestamp)
      .def_readwrite("gripper_pos", &JointState::gripper_pos)
      .def_readwrite("gripper_vel", &JointState::gripper_vel)
      .def_readwrite("gripper_torque", &JointState::gripper_torque)
      .def("__add__", [](const JointState& self,
                         const JointState& other) { return self + other; })
      .def("__mul__", [](const JointState& self,
                         const float& scalar) { return self * scalar; })
      .def("pos", &JointState::get_pos_ref, py::return_value_policy::reference)
      .def("vel", &JointState::get_vel_ref, py::return_value_policy::reference)
      .def("torque", &JointState::get_torque_ref,
           py::return_value_policy::reference);
  py::class_<EEFState>(m, "EEFState")
      .def(py::init<>())
      .def(py::init<Vec6d, double>())
      .def_readwrite("timestamp", &EEFState::timestamp)
      .def_readwrite("gripper_pos", &EEFState::gripper_pos)
      .def_readwrite("gripper_vel", &EEFState::gripper_vel)
      .def_readwrite("gripper_torque", &EEFState::gripper_torque)
      .def("__add__", [](const EEFState& self,
                         const EEFState& other) { return self + other; })
      .def("__mul__", [](const EEFState& self,
                         const float& scalar) { return self * scalar; })
      .def("pose_6d", &EEFState::get_pose_6d_ref,
           py::return_value_policy::reference);
  py::class_<Gain>(m, "Gain")
      .def(py::init<>())
      .def(py::init<Vec6d, Vec6d, double, double>())
      .def_readwrite("gripper_kp", &Gain::gripper_kp)
      .def_readwrite("gripper_kd", &Gain::gripper_kd)
      .def("__add__",
           [](const Gain& self, const Gain& other) { return self + other; })
      .def("__mul__",
           [](const Gain& self, const float& scalar) { return self * scalar; })
      .def("kp", &Gain::get_kp_ref, py::return_value_policy::reference)
      .def("kd", &Gain::get_kd_ref, py::return_value_policy::reference);
  py::class_<Arx5JointController>(m, "Arx5JointController")
      .def(py::init<const std::string&, const std::string&>())
      .def("send_recv_once", &Arx5JointController::send_recv_once)
      .def("enable_background_send_recv",
           &Arx5JointController::enable_background_send_recv)
      .def("disable_background_send_recv",
           &Arx5JointController::disable_background_send_recv)
      .def("enable_gravity_compensation",
           &Arx5JointController::enable_gravity_compensation)
      .def("disable_gravity_compensation",
           &Arx5JointController::disable_gravity_compensation)
      .def("get_state", &Arx5JointController::get_state)
      .def("get_timestamp", &Arx5JointController::get_timestamp)
      .def("set_joint_cmd", &Arx5JointController::set_joint_cmd)
      .def("get_joint_cmd", &Arx5JointController::get_joint_cmd)
      .def("set_gain", &Arx5JointController::set_gain)
      .def("get_gain", &Arx5JointController::get_gain)
      .def("get_robot_config", &Arx5JointController::get_robot_config)
      .def("reset_to_home", &Arx5JointController::reset_to_home)
      .def("set_to_damping", &Arx5JointController::set_to_damping)
      .def("set_log_level", &Arx5JointController::set_log_level)
      .def("calibrate_joint", &Arx5JointController::calibrate_joint)
      .def("calibrate_gripper", &Arx5JointController::calibrate_gripper);
  py::class_<Arx5CartesianController>(m, "Arx5CartesianController")
      .def(py::init<const std::string&, const std::string&,
                    const std::string&>())
      .def("set_eef_cmd", &Arx5CartesianController::set_eef_cmd)
      .def("get_eef_cmd", &Arx5CartesianController::get_eef_cmd)
      .def("get_joint_cmd", &Arx5CartesianController::get_joint_cmd)
      .def("get_eef_state", &Arx5CartesianController::get_eef_state)
      .def("get_joint_state", &Arx5CartesianController::get_joint_state)
      .def("get_timestamp", &Arx5CartesianController::get_timestamp)
      .def("set_gain", &Arx5CartesianController::set_gain)
      .def("get_gain", &Arx5CartesianController::get_gain)
      .def("set_log_level", &Arx5CartesianController::set_log_level)
      .def("get_robot_config", &Arx5CartesianController::get_robot_config)
      .def("reset_to_home", &Arx5CartesianController::reset_to_home)
      .def("set_to_damping", &Arx5CartesianController::set_to_damping);
  py::class_<Arx5Solver>(m, "Arx5Solver")
      .def(py::init<const std::string&>())
      .def("inverse_dynamics", &Arx5Solver::inverse_dynamics)
      .def("forward_kinematics", &Arx5Solver::forward_kinematics)
      .def("inverse_kinematics", &Arx5Solver::inverse_kinematics);
  py::class_<RobotConfig>(m, "RobotConfig")
      .def(py::init<const std::string&, double>())
      .def_readwrite("joint_pos_min", &RobotConfig::joint_pos_min)
      .def_readwrite("joint_pos_max", &RobotConfig::joint_pos_max)
      .def_readwrite("default_kp", &RobotConfig::default_kp)
      .def_readwrite("default_kd", &RobotConfig::default_kd)
      .def_readwrite("joint_vel_max", &RobotConfig::joint_vel_max)
      .def_readwrite("joint_torque_max", &RobotConfig::joint_torque_max)
      .def_readwrite("ee_vel_max", &RobotConfig::ee_vel_max)
      .def_readwrite("gripper_vel_max", &RobotConfig::gripper_vel_max)
      .def_readwrite("gripper_torque_max", &RobotConfig::gripper_torque_max)
      .def_readwrite("gripper_width", &RobotConfig::gripper_width)
      .def_readwrite("default_gripper_kp", &RobotConfig::default_gripper_kp)
      .def_readwrite("default_gripper_kd", &RobotConfig::default_gripper_kd)
      .def_readwrite("over_current_cnt_max", &RobotConfig::over_current_cnt_max)
      .def_readwrite("gripper_open_readout", &RobotConfig::gripper_open_readout)
      .def_readwrite("joint_dof", &RobotConfig::joint_dof)
      .def_readwrite("motor_id", &RobotConfig::motor_id)
      .def_readwrite("model", &RobotConfig::model)
      .def_readwrite("controller_dt", &RobotConfig::controller_dt)
      .def_readwrite("motor_type", &RobotConfig::motor_type);
  py::enum_<MotorType>(m, "MotorType")
      .value("EC_A4310", MotorType::EC_A4310)
      .value("DM_J4310", MotorType::DM_J4310)
      .value("DM_J4340", MotorType::DM_J4340);
}