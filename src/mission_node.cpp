/*
* mission_node.cpp
*
* ---------------------------------------------------------------------
* Copyright (C) 2023 Matthew (matthewoots at gmail.com)
*
*  This program is free software; you can redistribute it and/or
*  modify it under the terms of the GNU General Public License
*  as published by the Free Software Foundation; either version 2
*  of the License, or (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
* ---------------------------------------------------------------------
*/

#include <memory>
#include <vector>
#include <regex>
#include <mutex>
#include <queue>
#include <string>
#include <sstream>

#include <Eigen/Dense>

#include "crazyswarm_application/msg/user_command.hpp"
#include "crazyswarm_application/msg/agents_state_feedback.hpp"
#include "crazyswarm_application/msg/agent_state.hpp"

#include <rclcpp/rclcpp.hpp>
#include "common.h"

using crazyswarm_application::msg::UserCommand;
using crazyswarm_application::msg::AgentsStateFeedback;
using crazyswarm_application::msg::AgentState;

using std::placeholders::_1;
using std::placeholders::_2;

using namespace std::chrono_literals;

using namespace common;

class mission_handler : public rclcpp::Node
{
    private:

        rclcpp::Clock clock;

        struct commander
        {
            std::string task;
            std::string cont; // continuity
            std::vector<std::string> agents; 
            Eigen::Vector4d target;
            double duration; // s
            bool sent_mission;
        };

        double external_msg_threshold = 5.0;

        string_dictionary dict;
        
        std::queue<commander> command_sequence;

        std::vector<commander> command_buffer;

        rclcpp::Time last_mission_time;

        rclcpp::Time last_external_command_time;

        rclcpp::Publisher<UserCommand>::SharedPtr command_publisher;

        rclcpp::Subscription<AgentsStateFeedback>::SharedPtr agent_state_subscription;

        rclcpp::Subscription<UserCommand>::SharedPtr external_command_subscription;

        std::map<std::string, agent_state> agents_description;

        std::queue<commander> external_command_queue;

    public:

        // uint8 IDLE = 0 # Have not taken off
        // uint8 TAKEOFF = 1 # Taking off sequence
        // uint8 MOVE = 2 # Move according to external command (change target)
        // uint8 INTERNAL_TRACKING = 3 # Internal command logic takes precedence over external
        // uint8 HOVER = 4 # Stop and hover
        // uint8 LAND = 5 # Landing sequence

        mission_handler()
            : Node("mission_handler"), clock(RCL_ROS_TIME)
        {
            RCLCPP_INFO(this->get_logger(), "start constructor");

            last_mission_time = clock.now();

            this->declare_parameter("command_sequence");
            std::vector<std::string> command_vector = 
                this->get_parameter("command_sequence").get_parameter_value().get<std::vector<std::string>>();
            
            // number of segments per command
            int command_segments = 5;

            if (command_vector.size() % command_segments != 0)
                throw std::invalid_argument("[mission input] command_vector arguments must be divisible by command_segments");
            
            RCLCPP_INFO(this->get_logger(), "command_vector size %ld, segments %ld",
                command_vector.size(), command_vector.size() / command_segments);

            for (size_t i = 0; i < command_vector.size(); i += command_segments)
            {
                commander cmd;
                cmd.task = command_vector[i+0];
                cmd.cont = command_vector[i+1];

                std::string acc_string;
                std::vector<std::string> agents_included = 
                    split_space_delimiter(command_vector[i+2]);

                for (size_t j = 0; j < agents_included.size(); j++)
                    acc_string += agents_included[j];

                cmd.agents = agents_included;

                if (strcmp(cmd.task.c_str(), dict.hold.c_str()) == 0)
                    if (command_vector[i+3].empty())
                        throw std::invalid_argument("[mission input] invalid hold format");
                    else
                        cmd.duration = std::stod(command_vector[i+3].c_str());
                else
                    cmd.duration = 0.0;

                if (strcmp(cmd.task.c_str(), dict.go_to.c_str()) == 0 || 
                    strcmp(cmd.task.c_str(), dict.go_to_velocity.c_str()) == 0)
                {
                    if (command_vector[i+4].empty())
                        throw std::invalid_argument("[mission input] invalid goto or goto velocity target format");
                    else
                    {
                        std::vector<std::string> targets = 
                            split_space_delimiter(command_vector[i+4]);

                        if (targets.size() != 4)
                            throw std::invalid_argument("[mission input] target not in xyz format");

                        cmd.target[0] = std::stod(targets[0]);
                        cmd.target[1] = std::stod(targets[1]);
                        cmd.target[2] = std::stod(targets[2]);
                        cmd.target[3] = std::stod(targets[3]);
                    }
                }
                else
                    cmd.target = Eigen::Vector4d::Zero();
                    
                cmd.sent_mission = false;

                command_sequence.push(cmd);

                RCLCPP_INFO(this->get_logger(), "task %s, cont %s, agent %s, duration %.3lfs, target [%.3lf %.3lf %.3lf]",
                    cmd.task.c_str(), cmd.cont.c_str(), 
                    acc_string.c_str(), cmd.duration, 
                    cmd.target.x(), cmd.target.y(), cmd.target.z());
            }

            command_publisher = 
                this->create_publisher<UserCommand>("user", 5);
            agent_state_subscription = 
                this->create_subscription<AgentsStateFeedback>("agents", 
                2, std::bind(&mission_handler::agent_event_callback, this, _1));

            // load crazyflies from params
            auto node_parameters_iface = this->get_node_parameters_interface();
            const std::map<std::string, rclcpp::ParameterValue> &parameter_overrides =
                node_parameters_iface->get_parameter_overrides();
            auto cf_names = extract_names(parameter_overrides, "robots");

            external_command_subscription = 
                this->create_subscription<UserCommand>("/user/external", 
                10, std::bind(&mission_handler::external_command_callback, this, _1));

            for (const auto &name : cf_names) 
            {
                agent_state state;
                state.t = clock.now();
                state.transform = Eigen::Affine3d::Identity();
                state.velocity = Eigen::Vector3d::Zero();
                state.flight_state = IDLE;
                state.radio_connection = false;

                agents_description.insert(
                    std::pair<std::string, agent_state>(name, state));
            }

            RCLCPP_INFO(this->get_logger(), "end_constructor");
        }

        void external_command_callback(const UserCommand::SharedPtr msg)
        {
            commander ext;
            ext.task = "goto_velocity";

            for (std::string agent : msg->uav_id)
                ext.agents.push_back(agent);
            
            ext.target = Eigen::Vector4d(
                msg->goal.x, msg->goal.y, msg->goal.z, msg->yaw
            );

            external_command_queue.push(ext);
        }

        void agent_event_callback(const AgentsStateFeedback::SharedPtr msg)
        {            
            rclcpp::Time now = clock.now();

            AgentsStateFeedback copy = *msg;
            // copy agent messages into local states
            for (auto &agent : copy.agents)
            {
                // check agents_description queue
                std::map<std::string, agent_state>::iterator it = 
                    agents_description.find(agent.id);

                if (it == agents_description.end())
                    continue;
                
                it->second.flight_state = agent.flight_state;
                it->second.radio_connection = agent.connected;
                it->second.completed = agent.completed;
            }

            // check through the agent's states
            // (1) Check for radio
            // (2) Check whether task is completed
            size_t all_connected = 0;
            for (auto &agent : agents_description)
            {
                if (agent.second.radio_connection)
                    all_connected++;
                call_state_printer(agent);
            }
            std::cout << std::endl;

            // if all are not connected do not continue the task
            if (all_connected != agents_description.size())
                return;

            // command handler
            if (!command_buffer.empty())
            {
                size_t success_count = 0;
                for (auto cmd = std::begin(command_buffer); 
                    cmd != std::end(command_buffer); ++cmd) 
                {
                    // check for hold command
                    if (strcmp(cmd->task.c_str(), dict.hold.c_str()) == 0)
                    {
                        double duration_seconds = (clock.now() - last_mission_time).seconds();
                        if (cmd->duration < duration_seconds)
                        {
                            RCLCPP_INFO(this->get_logger(), "waiting over %.3lf/%.3lfs", 
                                duration_seconds, cmd->duration);
                            success_count++;
                        }
                        else 
                        {
                            RCLCPP_INFO(this->get_logger(), "waiting %.3lf/%.3lfs", 
                                duration_seconds, cmd->duration);
                            return;
                        }
                    }
                    else
                    {
                        if (strcmp(cmd->task.c_str(), dict.external.c_str()) == 0)
                        {
                            if ((clock.now() - last_external_command_time).seconds() > 
                                external_msg_threshold)
                                success_count++;
                            else
                                RCLCPP_INFO(this->get_logger(), "external time %lf/%lf", 
                                    (clock.now() - last_external_command_time).seconds(), 
                                    external_msg_threshold);
                            continue;
                        }
                        if (check_completed_agents(cmd))
                            success_count++;
                    }
                }

                RCLCPP_INFO(this->get_logger(), "success %ld/%ld", 
                    success_count, command_buffer.size());
                if (success_count == command_buffer.size())
                {
                    last_mission_time = clock.now();
                    command_buffer.clear();
                }
            }
            
            // if all have completed the task move to the next task
            // check whether the agents inside the command sequence are completed
            if (command_buffer.empty() && !command_sequence.empty())
            {
                while (!command_sequence.empty())
                {
                    // empty string or task rejection
                    if (command_sequence.front().task.empty())
                        command_sequence.pop();
                    else
                    {
                        command_buffer.emplace_back(command_sequence.front());
                        if (strcmp(command_sequence.front().task.c_str(), dict.external.c_str()) == 0)
                            last_external_command_time = clock.now();
                        command_sequence.pop();
                    }
                    if (strcmp(command_buffer.back().cont.c_str(), dict.wait.c_str()) == 0)
                        break;
                }
            }

            // if empty sequence and buffer left, end the node
            if (command_buffer.empty() && command_sequence.empty())
            {
                // close the mission node when we have finished
                RCLCPP_INFO(this->get_logger(), "It's been a long day without you, my friend");
                RCLCPP_INFO(this->get_logger(), "And I'll tell you all about it when I see you again");
                rclcpp::shutdown();
                return;
            }

            for (auto cmd = std::begin(command_buffer); 
                cmd != std::end(command_buffer); ++cmd) 
            {
                // if task is already sent, do not send again
                if (cmd->sent_mission)
                    continue;

                if (cmd->agents.empty())
                    throw std::invalid_argument("empty agent list");

                // "takeoff"
                if (strcmp(cmd->task.c_str(), dict.takeoff.c_str()) == 0) 
                {
                    // "all"
                    if (strcmp(cmd->agents[0].c_str(), dict.all.c_str()) == 0)
                    {
                        UserCommand command;
                        command.cmd = "takeoff_all";
                        command_publisher->publish(command);

                        cmd->sent_mission = true;

                        RCLCPP_INFO(this->get_logger(), "Sent %s takeoff", 
                            cmd->agents[0].c_str());
                        return;
                    }

                    // individual
                    UserCommand command;
                    command.cmd = "takeoff";
                    std::string acc_id;
                    for (auto &agent : cmd->agents)
                    {
                        std::map<std::string, agent_state>::iterator it = 
                            agents_description.find(agent);
                        
                        if (it == agents_description.end())
                            continue;
                        
                        command.uav_id.push_back(it->first);
                        acc_id += it->first;
                    }
                    
                    command_publisher->publish(command);
                    cmd->sent_mission = true;

                    RCLCPP_INFO(this->get_logger(), "Sent %s takeoff", acc_id.c_str());
                }

                // "goto_velocity"
                else if(strcmp(cmd->task.c_str(), 
                    dict.go_to_velocity.c_str()) == 0)
                {
                    UserCommand command;
                    command.cmd = "goto_velocity";
                    std::string acc_id;

                    // "all"
                    if (strcmp(cmd->agents[0].c_str(), dict.all.c_str()) == 0) 
                        for (auto &[key, state] : agents_description)
                        {
                            command.uav_id.push_back(key);
                            acc_id += key;
                        }
                    // "individual"
                    else
                    {
                        for (auto &agent : cmd->agents)
                        {
                            std::map<std::string, agent_state>::iterator it = 
                                agents_description.find(agent);
                            
                            if (it == agents_description.end())
                                continue;
                            
                            command.uav_id.push_back(it->first);
                            acc_id += it->first;
                        }
                    }
                        
                    command.goal.x = cmd->target[0];
                    command.goal.y = cmd->target[1];
                    command.goal.z = cmd->target[2];
                    command.yaw = cmd->target[3];
                    
                    command_publisher->publish(command);
                    cmd->sent_mission = true;
                    
                    RCLCPP_INFO(this->get_logger(), "Sent %s goto_velocity", 
                        acc_id.c_str());                    
                }

                // "goto"
                else if(strcmp(cmd->task.c_str(), dict.go_to.c_str()) == 0)
                {
                    UserCommand command;
                    command.cmd = "goto";
                    std::string acc_id;

                    // "all"
                    if (strcmp(cmd->agents[0].c_str(), dict.all.c_str()) == 0) 
                        for (auto &[key, state] : agents_description)
                        {
                            command.uav_id.push_back(key);
                            acc_id += key;
                        }
                    // "individual"
                    else
                    {
                        for (auto &agent : cmd->agents)
                        {
                            std::map<std::string, agent_state>::iterator it = 
                                agents_description.find(agent);
                            
                            if (it == agents_description.end())
                                continue;
                            
                            command.uav_id.push_back(it->first);
                            acc_id += it->first;
                        }
                    }
                        
                    command.goal.x = cmd->target[0];
                    command.goal.y = cmd->target[1];
                    command.goal.z = cmd->target[2];
                    command.yaw = cmd->target[3];
                    
                    command_publisher->publish(command);
                    cmd->sent_mission = true;
                    
                    RCLCPP_INFO(this->get_logger(), "Sent %s goto", 
                        acc_id.c_str());                    
                }

                // "land"
                else if(strcmp(cmd->task.c_str(), dict.land.c_str()) == 0)
                {
                    // "all"
                    if (strcmp(cmd->agents[0].c_str(), dict.all.c_str()) == 0)
                    {
                        UserCommand command;
                        command.cmd = "land_all";
                        command_publisher->publish(command);
                        cmd->sent_mission = true;

                        RCLCPP_INFO(this->get_logger(), "Sent %s land", 
                            cmd->agents[0].c_str());
                        return;
                    }

                    // individual
                    UserCommand command;
                    command.cmd = "land";
                    std::string acc_id;
                    for (auto &agent : cmd->agents)
                    {
                        std::map<std::string, agent_state>::iterator it = 
                            agents_description.find(agent);
                        
                        if (it == agents_description.end())
                            continue;
                        
                        command.uav_id.push_back(it->first);
                        acc_id += it->first;
                    }

                    command_publisher->publish(command);
                    cmd->sent_mission = true;

                    RCLCPP_INFO(this->get_logger(), "Sent %s land", acc_id.c_str());
                }

                // "external"
                else if(strcmp(cmd->task.c_str(), dict.external.c_str()) == 0)
                {
                    while (!external_command_queue.empty())
                    {
                        UserCommand command;
                        command.cmd = external_command_queue.front().task;
                        std::string acc_id;
                        // "individual"
                        for (auto &agent : external_command_queue.front().agents)
                        {
                            std::map<std::string, agent_state>::iterator it = 
                                agents_description.find(agent);
                            
                            if (it == agents_description.end())
                                continue;
                            
                            command.uav_id.push_back(it->first);
                            acc_id += it->first;
                        }
                            
                        command.goal.x = external_command_queue.front().target[0];
                        command.goal.y = external_command_queue.front().target[1];
                        command.goal.z = external_command_queue.front().target[2];
                        command.yaw = external_command_queue.front().target[3];
                        command.is_external = true;
                        
                        command_publisher->publish(command);
                        // cmd->sent_mission = true;
                        
                        RCLCPP_INFO(this->get_logger(), "Sent %s external", 
                            acc_id.c_str());
                        
                        external_command_queue.pop();

                        last_external_command_time = clock.now();
                    }
                }
            }
        }

        void call_state_printer(std::pair<std::string, agent_state> agent)
        {
            std::cout << agent.first << " (rc:" << 
                (agent.second.radio_connection ? "y" : "n") << ") (tsk:" <<
                (agent.second.completed ? "done" : "not-done") << ") ";
            switch (agent.second.flight_state)
            {
                case IDLE:
                    std::cout << "IDLE";
                    break;
                case TAKEOFF:
                    std::cout << "TAKEOFF";
                    break;
                case MOVE:
                    std::cout << "MOVE";
                    break;
                case MOVE_VELOCITY:
                    std::cout << "MOVE_VELOCITY";
                    break;
                case INTERNAL_TRACKING:
                    std::cout << "INTERNAL_TRACKING";
                    break;
                case HOVER:
                    std::cout << "HOVER";
                    break;
                case LAND:
                    std::cout << "LAND";
                    break;
                default:
                    std::cout << "ERROR";
                    break;
            }

            std::cout << " | ";
        }

        bool check_completed_agents(
            std::vector<commander>::iterator command)
        {
            // all
            if (strcmp(command->agents[0].c_str(), dict.all.c_str()) == 0)
            {
                // check all agents
                for (auto &[key, state] : agents_description)
                {
                    if (!state.completed)
                        return false;
                }
            }
            // individual
            else
            {
                for (auto &agent : command->agents)
                {
                    auto iterator = agents_description.find(agent);
                    if (iterator == agents_description.end())
                        return false;
                    
                    if (!(iterator->second.completed))
                        return false;
                }
            }

            return true;
        }

};

int main(int argc, char *argv[])
{
    rclcpp::init(argc, argv);

    size_t thread_count = 1;
    rclcpp::executors::MultiThreadedExecutor 
        executor(rclcpp::ExecutorOptions(), thread_count, false);
    auto node = std::make_shared<mission_handler>();
    executor.add_node(node);
    executor.spin();
    rclcpp::shutdown();
    
    return 0;
}