/*
* crazyswarm_app.cpp
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

#include "crazyswarm_app.h"

int main(int argc, char *argv[])
{
    rclcpp::init(argc, argv);

    size_t thread_count = 5;
    rclcpp::executors::MultiThreadedExecutor 
        executor(rclcpp::ExecutorOptions(), thread_count, false);
    auto node = std::make_shared<cs2::cs2_application>();
    executor.add_node(node);
    executor.spin();
    rclcpp::shutdown();
    
    return 0;
}

void cs2::cs2_application::user_callback(
    const UserCommand::SharedPtr msg)
{
    RCLCPP_INFO(this->get_logger(), "received command");
    using namespace cs2;
    UserCommand copy = *msg;
    string_dictionary dict;

    // handle goto_velocity
    if (strcmp(copy.cmd.c_str(), 
        dict.go_to_velocity.c_str()) == 0)
    {
        for (size_t i = 0; i < copy.uav_id.size(); i++)
        {           
            auto iterator_states = agents_states.find(copy.uav_id[i]);
            if (iterator_states == agents_states.end())
                continue;

            // check if the command is external, if so, keep changing the goal
            if (copy.is_external)
                 while (!iterator_states->second.target_queue.empty())
                    iterator_states->second.target_queue.pop();

            iterator_states->second.target_queue.push(
                Eigen::Vector3d(copy.goal.x, copy.goal.y, copy.goal.z)
            );

            iterator_states->second.flight_state = MOVE_VELOCITY;
            iterator_states->second.completed = false;
        }
    }
    // handle takeoff_all and land_all
    else if (strcmp(copy.cmd.c_str(), dict.takeoff_all.c_str()) == 0 || 
        strcmp(copy.cmd.c_str(), dict.land_all.c_str()) == 0)
    {
        bool is_takeoff_all = 
            strcmp(copy.cmd.c_str(), dict.takeoff_all.c_str()) == 0;

        RCLCPP_INFO(this->get_logger(), "%s", is_takeoff_all ? "takeoff request all" : "land request all");

        auto start = clock.now();
        if (is_takeoff_all)
        {
            auto request = std::make_shared<Takeoff::Request>();
            request->group_mask = 0;
            request->height = takeoff_height;
            
            double sec = std::floor(takeoff_height / takeoff_land_velocity);
            double nanosec = (takeoff_height / takeoff_land_velocity - std::floor(takeoff_height / takeoff_land_velocity)) * 1e9;

            request->duration.sec = sec;
            request->duration.nanosec = nanosec;
                
            // while (!takeoff_all_client->wait_for_service())
            //     RCLCPP_INFO(this->get_logger(), "service not available, waiting again...");
            
            auto result = takeoff_all_client->async_send_request(request); 
            // auto response = result.get();

            // Iterate through the agents
            for (auto &[key, agent] : agents_states)
            {
                Eigen::Vector3d trans = agent.transform.translation();
                
                while (!agent.target_queue.empty())
                    agent.target_queue.pop();

                agent.target_queue.push(
                    Eigen::Vector3d(trans.x(), trans.y(), takeoff_height)
                );
                agent.flight_state = is_takeoff_all ? TAKEOFF : LAND;
                agent.completed = false;
            }
        }
        else
        {
            auto request = std::make_shared<Land::Request>();
            request->group_mask = 0;
            request->height = 0.0;

            double sec = std::floor(takeoff_height / takeoff_land_velocity);
            double nanosec = (takeoff_height / takeoff_land_velocity - std::floor(takeoff_height / takeoff_land_velocity)) * 1e9;

            request->duration.sec = sec;
            request->duration.nanosec = nanosec;

            // while (!land_all_client->wait_for_service()) 
            //     RCLCPP_INFO(this->get_logger(), "service not available, waiting again...");
            
            auto result = land_all_client->async_send_request(request);
            // auto response = result.get();

            // Iterate through the agents
            for (auto &[key, agent] : agents_states)
            {
                Eigen::Vector3d trans = agent.transform.translation();
                
                while (!agent.target_queue.empty())
                    agent.target_queue.pop();

                agent.target_queue.push(
                    Eigen::Vector3d(trans.x(), trans.y(), 0.0)
                );
                agent.flight_state = is_takeoff_all ? TAKEOFF : LAND;
                agent.completed = false;
            }
        }

        RCLCPP_INFO(this->get_logger(), "%s (%lfms)", 
            is_takeoff_all ? "takeoff_all_sent" : "land_all_sent", (clock.now() - start).seconds()*1000.0);
    }
    // handle land and go_to
    else if (strcmp(copy.cmd.c_str(), dict.land.c_str()) == 0 || 
        strcmp(copy.cmd.c_str(), dict.go_to.c_str()) == 0)
    {
        bool is_go_to = strcmp(copy.cmd.c_str(), dict.go_to.c_str()) == 0;

        std::queue<int> check_queue;
        for (size_t i = 0; i < copy.uav_id.size(); i++)
        {
            // get position and distance
            auto iterator = agents_comm.find(copy.uav_id[i]);
            if (iterator == agents_comm.end())
                continue;
            
            auto iterator_states = agents_states.find(copy.uav_id[i]);
            if (iterator_states == agents_states.end())
                continue;
            
            if (!is_go_to)
            {
                auto start = clock.now();

                send_land_and_update(iterator_states, iterator);

                RCLCPP_INFO(this->get_logger(), "land sent for %s and changing group_mask (%lfms)", 
                    iterator->first.c_str(), (clock.now() - start).seconds()*1000.0);
                
                check_queue.push(i);
            }
            else
            {
                auto start = clock.now();

                auto request = std::make_shared<GoTo::Request>();
                double distance = 
                    (iterator_states->second.transform.translation() - 
                    Eigen::Vector3d(copy.goal.x, copy.goal.y, copy.goal.z)).norm();
                request->group_mask = 0;
                request->relative = false;
                request->goal = copy.goal;
                request->yaw = copy.yaw;

                double sec = std::floor(distance / max_velocity);
                double nanosec = (distance / max_velocity - std::floor(distance / max_velocity)) * 1e9;

                request->duration.sec = sec;
                request->duration.nanosec = nanosec;

                auto result = 
                    iterator->second.go_to->async_send_request(request);
                check_queue.push(i);

                while (!iterator_states->second.target_queue.empty())
                    iterator_states->second.target_queue.pop();

                iterator_states->second.flight_state = MOVE;
                iterator_states->second.completed = false;

                RCLCPP_INFO(this->get_logger(), "go_to sent for %s (%lfms)", 
                    iterator->first.c_str(), (clock.now() - start).seconds()*1000.0);
            }
        }

        if (check_queue.size() != copy.uav_id.size())
        {
            while (!check_queue.empty())
            {
                copy.uav_id.erase(copy.uav_id.begin() + (check_queue.front()-1));
                check_queue.pop();
            }
            
            std::cout << "uav_id not found ";
            for (const auto& i: copy.uav_id)
                std::cout << i << " ";
            std::cout << std::endl;

            RCLCPP_INFO(this->get_logger(), "%s", is_go_to ? 
                "go_to_sent unfinished" : "land_sent unfinished");
            return;
        }
        else
            RCLCPP_INFO(this->get_logger(), "%s", is_go_to ? 
                "go_to_sent" : "land_sent");
    }
    else
        RCLCPP_ERROR(this->get_logger(), "wrong command type, resend");

}

void cs2::cs2_application::pose_callback(
    const PoseStamped::SharedPtr msg, 
    std::map<std::string, agent_state>::iterator state)
{
    agent_update_mutex.lock();

    PoseStamped copy = *msg;
    auto pos = state->second.transform.translation();
    // RCLCPP_INFO(this->get_logger(), "(%s) %lf %lf %lf", state->first.c_str(), 
    //     pos[0], pos[1], pos[2]);
    state->second.transform.translation() = 
        Eigen::Vector3d(copy.pose.position.x, copy.pose.position.y, copy.pose.position.z);
    Eigen::Quaterniond q = Eigen::Quaterniond(
        copy.pose.orientation.w, copy.pose.orientation.x,
        copy.pose.orientation.y, copy.pose.orientation.z);
    state->second.transform.linear() = q.toRotationMatrix();
    state->second.t = copy.header.stamp;

    // check agents_tag_queue and update s_queue
    std::map<std::string, tag_queue>::iterator it = 
        agents_tag_queue.find(state->first);
    
    if (it != agents_tag_queue.end())
    {
        if (it->second.s_queue.size() > max_queue_size)
            it->second.s_queue.pop();
        it->second.s_queue.push(state->second);
    }

    state->second.radio_connection = true;

    agent_update_mutex.unlock();
}

void cs2::cs2_application::twist_callback(
    const Twist::SharedPtr msg, 
    std::map<std::string, agent_state>::iterator state)
{
    agent_update_mutex.lock();

    Twist copy = *msg;
    // Eigen::Vector3d pos = pose.second.translation();
    // RCLCPP_INFO(this->get_logger(), "(%ld) %lf %lf %lf", pose.first, 
    //     pos[0], pos[1], pos[2]);
    state->second.velocity = 
        Eigen::Vector3d(copy.linear.x, copy.linear.y, copy.linear.z);
    
    agent_update_mutex.unlock();
}

void cs2::cs2_application::send_land_and_update(
    std::map<std::string, agent_state>::iterator s,
    std::map<std::string, agent_struct>::iterator c)
{
    auto request_land = std::make_shared<Land::Request>();
    request_land->group_mask = 0;
    request_land->height = 0.0;

    agent_update_mutex.lock();

    double sec = std::floor(s->second.transform.translation().z() / takeoff_land_velocity);
    double nanosec = (s->second.transform.translation().z() / takeoff_land_velocity - 
        std::floor(s->second.transform.translation().z() / takeoff_land_velocity)) * 1e9;

    request_land->duration.sec = sec;
    request_land->duration.nanosec = nanosec;

    auto result_land = 
        c->second.land->async_send_request(request_land);
    
    auto request_group = std::make_shared<SetGroupMask::Request>();
    request_group->group_mask = 1;
    auto result_group = 
        c->second.set_group->async_send_request(request_group);
    
    Eigen::Vector3d trans = s->second.transform.translation();

    while (!s->second.target_queue.empty())
        s->second.target_queue.pop();

    s->second.target_queue.push(
        Eigen::Vector3d(trans.x(), trans.y(), 0.0)
    );

    agent_update_mutex.unlock();

    s->second.flight_state = LAND;
    s->second.completed = false;
}