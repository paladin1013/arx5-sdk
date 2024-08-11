#ifndef CONFIG_H
#define CONFIG_H

#include "app/common.h"
#include <memory>
#include <unordered_map>
#include <vector>
namespace arx
{

class RobotConfig
{
  public:
    std::string robot_model;

    VecDoF joint_pos_min;
    VecDoF joint_pos_max;
    VecDoF joint_vel_max;    // rad/s
    VecDoF joint_torque_max; // N*m
    VecDoF ee_vel_max;
    // end effector speed: m/s for (x, y, z), rad/s for (roll, pitch, yaw)

    double gripper_vel_max; // m/s
    double gripper_torque_max;
    double gripper_width; // fully opened: GRIPPER_WIDTH, fully closed: 0
    double gripper_open_readout;
    int joint_dof;
    std::vector<int> motor_id;
    std::vector<MotorType> motor_type;
    int gripper_motor_id;
    MotorType gripper_motor_type; // Set to MotorType::None if the robot does not have a gripper

    // Will be used in inverse dynamics calculation.
    // Please change it to other values if the robot arm is not placed on the ground.
    Eigen::Vector3d gravity_vector;

    // Will be used in IK and FK.
    // ID will find stop at last active joint (instead of the eef link with a fixed joint) because of some KDL bugs
    std::string base_link_name;
    std::string eef_link_name;

    RobotConfig(std::string robot_model, VecDoF joint_pos_min, VecDoF joint_pos_max, VecDoF joint_vel_max,
                VecDoF joint_torque_max, VecDoF ee_vel_max, double gripper_vel_max, double gripper_torque_max,
                double gripper_width, double gripper_open_readout, int joint_dof, std::vector<int> motor_id,
                std::vector<MotorType> motor_type, int gripper_motor_id, MotorType gripper_motor_type,
                Eigen::Vector3d gravity_vector, std::string base_link_name, std::string eef_link_name)
        : robot_model(robot_model), joint_pos_min(joint_pos_min), joint_pos_max(joint_pos_max),
          joint_vel_max(joint_vel_max), joint_torque_max(joint_torque_max), ee_vel_max(ee_vel_max),
          gripper_vel_max(gripper_vel_max), gripper_torque_max(gripper_torque_max), gripper_width(gripper_width),
          gripper_open_readout(gripper_open_readout), joint_dof(joint_dof), motor_id(motor_id), motor_type(motor_type),
          gripper_motor_id(gripper_motor_id), gripper_motor_type(gripper_motor_type), gravity_vector(gravity_vector),
          base_link_name(base_link_name), eef_link_name(eef_link_name)
    {
    }
};

class ControllerConfig
{
  public:
    std::string controller_type;
    VecDoF default_kp;
    VecDoF default_kd;
    double default_gripper_kp;
    double default_gripper_kd;
    int over_current_cnt_max;
    double controller_dt;

    ControllerConfig(std::string controller_type, VecDoF default_kp, VecDoF default_kd, double default_gripper_kp,
                     double default_gripper_kd, int over_current_cnt_max, double controller_dt)
        : controller_type(controller_type), default_kp(default_kp), default_kd(default_kd),
          default_gripper_kp(default_gripper_kp), default_gripper_kd(default_gripper_kd),
          over_current_cnt_max(over_current_cnt_max), controller_dt(controller_dt)
    {
    }
};

class RobotConfigFactory
{
  public:
    static RobotConfigFactory &get_instance()
    {
        static RobotConfigFactory instance;
        return instance;
    }

    std::shared_ptr<RobotConfig> get_config(const std::string &robot_model)
    {
        auto it = configurations.find(robot_model);
        if (it != configurations.end())
        {
            return it->second;
        }
        else
        {
            throw std::runtime_error("Unknown robot model. Currently available: X5, L5");
        }
    }

  private:
    RobotConfigFactory()
    {
        configurations["X5"] = std::make_shared<RobotConfig>(
            "X5",                                                          // robot_model
            (VecDoF(6) << -3.14, -0.05, -0.1, -1.6, -1.57, -2).finished(), // joint_pos_min
            (VecDoF(6) << 2.618, 3.14, 3.24, 1.55, 1.57, 2).finished(),    // joint_pos_max
            (VecDoF(6) << 3.0, 2.0, 2.0, 2.0, 3.0, 3.0).finished(),        // joint_vel_max
            (VecDoF(6) << 30.0, 40.0, 30.0, 15.0, 10.0, 10.0).finished(),  // joint_torque_max
            (VecDoF(6) << 0.6, 0.6, 0.6, 1.8, 1.8, 1.8).finished(),        // ee_vel_max
            0.1,                                                           // gripper_vel_max
            1.5,                                                           // gripper_torque_max
            0.085,                                                         // gripper_width
            4.8,                                                           // gripper_open_readout
            6,                                                             // joint_dof
            std::vector<int>{1, 2, 4, 5, 6, 7},                            // motor_id
            std::vector<MotorType>{MotorType::EC_A4310, MotorType::EC_A4310, MotorType::EC_A4310, MotorType::DM_J4310,
                                   MotorType::DM_J4310, MotorType::DM_J4310}, // motor_type
            8,                                                                // gripper_motor_id
            MotorType::DM_J4310,                                              // gripper_motor_type
            (Eigen::Vector3d() << 0, 0, -9.807).finished(),                   // gravity_vector
            "base_link",                                                      // base_link_name
            "eef_link"                                                        // eef_link_name
        );
        configurations["L5"] = std::make_shared<RobotConfig>(
            "L5",                                                          // robot_model
            (VecDoF(6) << -3.14, -0.05, -0.1, -1.6, -1.57, -2).finished(), // joint_pos_min
            (VecDoF(6) << 2.618, 3.14, 3.24, 1.55, 1.57, 2).finished(),    // joint_pos_max
            (VecDoF(6) << 3.0, 2.0, 2.0, 2.0, 3.0, 3.0).finished(),        // joint_vel_max
            (VecDoF(6) << 30.0, 40.0, 30.0, 15.0, 10.0, 10.0).finished(),  // joint_torque_max
            (VecDoF(6) << 0.6, 0.6, 0.6, 1.8, 1.8, 1.8).finished(),        // ee_vel_max
            0.1,                                                           // gripper_vel_max
            1.5,                                                           // gripper_torque_max
            0.085,                                                         // gripper_width
            4.8,                                                           // gripper_open_readout
            6,                                                             // joint_dof
            std::vector<int>{1, 2, 4, 5, 6, 7},                            // motor_id
            std::vector<MotorType>{MotorType::DM_J4340, MotorType::DM_J4340, MotorType::DM_J4340, MotorType::DM_J4310,
                                   MotorType::DM_J4310, MotorType::DM_J4310}, // motor_type
            8,                                                                // gripper_motor_id
            MotorType::DM_J4310,                                              // gripper_motor_type
            (Eigen::Vector3d() << 0, 0, -9.807).finished(),                   // gravity_vector
            "base_link",                                                      // base_link_name
            "eef_link"                                                        // eef_link_name
        );
    }

    std::unordered_map<std::string, std::shared_ptr<RobotConfig>> configurations;

    // Disable copy constructor and assignment operator
    RobotConfigFactory(const RobotConfigFactory &) = delete;
    RobotConfigFactory &operator=(const RobotConfigFactory &) = delete;
};

class ControllerConfigFactory
{
  public:
    static ControllerConfigFactory &get_instance()
    {
        static ControllerConfigFactory instance;
        return instance;
    }

    std::shared_ptr<ControllerConfig> get_config(const std::string &controller_type)
    {
        auto it = configurations.find(controller_type);
        if (it != configurations.end())
        {
            return it->second;
        }
        else
        {
            throw std::runtime_error(
                "Unknown controller type. Currently available: joint_controller, cartesian_controller");
        }
    }

  private:
    ControllerConfigFactory()
    {
        configurations["joint_controller"] = std::make_shared<ControllerConfig>(
            "joint_controller",                                           // controller_type
            (VecDoF(6) << 70.0, 70.0, 70.0, 30.0, 30.0, 20.0).finished(), // default_kp
            (VecDoF(6) << 2.0, 2.0, 2.0, 1.0, 1.0, 1.0).finished(),       // default_kd
            30.0,                                                         // default_gripper_kp
            0.2,                                                          // default_gripper_kd
            20,                                                           // over_current_cnt_max
            0.002                                                         // controller_dt
        );
        configurations["cartesian_controller"] = std::make_shared<ControllerConfig>(
            "cartesian_controller",                                          // controller_type
            (VecDoF(6) << 150.0, 150.0, 200.0, 60.0, 30.0, 30.0).finished(), // default_kp
            (VecDoF(6) << 5.0, 5.0, 5.0, 1.0, 1.0, 1.0).finished(),          // default_kd
            30.0,                                                            // default_gripper_kp
            0.2,                                                             // default_gripper_kd
            20,                                                              // over_current_cnt_max
            0.005                                                            // controller_dt
        );
    }
    std::unordered_map<std::string, std::shared_ptr<ControllerConfig>> configurations;

    // Disable copy constructor and assignment operator
    ControllerConfigFactory(const ControllerConfigFactory &) = delete;
    ControllerConfigFactory &operator=(const ControllerConfigFactory &) = delete;
};
} // namespace arx
#endif // CONFIG_H