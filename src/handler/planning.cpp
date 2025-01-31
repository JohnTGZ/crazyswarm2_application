/*
* planning.cpp
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

void cs2::cs2_application::conduct_planning(
    Eigen::Vector3d &desired, std::string mykey, agent_state state) 
{
    auto it = rvo_agents.find(mykey);
    if (it == rvo_agents.end())
        return;

    // Construct KD-tree
    kd_tree = kd_create(3);
    for (auto &[key, agent] : agents_states)
    { 
        if (strcmp(mykey.c_str(), key.c_str()) == 0)
            continue;
        Eval_agent *node = new Eval_agent;
        node->position_ = agent.transform.translation().cast<float>();
        node->velocity_ = agent.velocity.cast<float>();
        node->radius_ = (float)protected_zone;
        int v = kd_insert3(
            kd_tree, node->position_.x(), 
            node->position_.y(), node->position_.z(),
            node);
    }

    struct kdres *neighbours;
    neighbours = kd_nearest_range3(
        kd_tree, state.transform.translation().x(), 
        state.transform.translation().y(), 
        state.transform.translation().z(),
        communication_radius);

    float communication_radius_float = (float)communication_radius;

    // clear agent neighbour before adding in new neighbours and obstacles
    it->second.clearAgentNeighbor();

    while (!kd_res_end(neighbours))
    {
        double pos[3];
        Eval_agent *agent = (Eval_agent*)kd_res_item(neighbours, pos);
        
        it->second.insertAgentNeighbor(*agent, communication_radius_float);
        // store range query result so that we dont need to query again for rewire;
        kd_res_next(neighbours); // go to next in kd tree range query result
    }

    kd_free(kd_tree);

    if (!it->second.noNeighbours())
    {
        agent_update_mutex.lock();
        it->second.updateState(
            state.transform.translation().cast<float>(), 
            state.velocity.cast<float>(), 
            desired.cast<float>());
        agent_update_mutex.unlock();

        it->second.computeNewVelocity();
        Eigen::Vector3f new_desired = it->second.getVelocity();
        desired = new_desired.cast<double>();
    }
}

void cs2::cs2_application::handler_timer_callback() 
{
    AgentsStateFeedback agents_feedback;
    MarkerArray target_array;

    // Iterate through the agents
    for (auto &[key, agent] : agents_states)
    {
        rclcpp::Time now = this->get_clock()->now();
        switch (agent.flight_state)
        {
            case IDLE:
            {
                break;
            }

            case HOVER: 
            {
                double pose_difference = 
                    (agent.previous_target - agent.transform.translation()).norm();

                Eigen::Vector3d vel_target;
                if (pose_difference < max_velocity)
                    vel_target = 
                        (agent.previous_target - agent.transform.translation()); 
                else
                    vel_target = 
                        (agent.previous_target - agent.transform.translation()).normalized() * max_velocity;
                
                VelocityWorld vel_msg;
                vel_msg.header.stamp = clock.now();
                vel_msg.vel.x = vel_target.x();
                vel_msg.vel.y = vel_target.y();
                vel_msg.vel.z = vel_target.z();
                vel_msg.height = agent.previous_target.z();
                vel_msg.yaw = 0.0;
                auto it = agents_comm.find(key);
                if (it != agents_comm.end())
                    it->second.vel_world_publisher->publish(vel_msg);
                
                // agent.completed = false;
                break;
            }
            case TAKEOFF: case LAND: case MOVE:
            {
                bool is_land = 
                    (agent.flight_state == LAND);

                if (agent.target_queue.empty())
                {
                    // we do not need to handle the velocity here since:
                    // cffirmware land service handles it for us
                    // after popping the takeoff/goto queue till it is empty, change state to hover
                    agent.flight_state = is_land ? IDLE : HOVER;
                    agent.completed = true;
                    break;
                }
                double pose_difference = 
                    (agent.target_queue.front() - agent.transform.translation()).norm();
                if (pose_difference < reached_threshold)
                {
                    agent.previous_target = agent.target_queue.front();
                    agent.target_queue.pop();
                }
                break;
            }
            case MOVE_VELOCITY: case INTERNAL_TRACKING:
            {
                rclcpp::Time start = clock.now();

                if (agent.target_queue.empty())
                {
                    // move velocity
                    if (agent.flight_state == MOVE_VELOCITY)
                    {
                        agent.flight_state = HOVER;
                        agent.completed = true;
                    }
                    // internal tracking
                    else
                    {
                        auto it = agents_comm.find(key);
                        if (it == agents_comm.end())
                            continue;

                        send_land_and_update(agents_states.find(key), it);                        
                        agent.completed = true;
                    }
                    
                    break;
                }
                double pose_difference = 
                    (agent.target_queue.front() - agent.transform.translation()).norm();

                VelocityWorld vel_msg;
                Eigen::Vector3d vel_target;
                vel_msg.height = agent.target_queue.front().z();

                if (pose_difference < reached_threshold)
                {
                    vel_target = Eigen::Vector3d::Zero();
                    agent.previous_target = agent.target_queue.front();
                    agent.target_queue.pop();
                }
                else if (pose_difference < max_velocity)
                    vel_target = 
                        (agent.target_queue.front() - agent.transform.translation()); 
                else
                {
                    vel_target = 
                        (agent.target_queue.front() - agent.transform.translation()).normalized() * max_velocity;
                    conduct_planning(vel_target, key, agent);
                }

                double duration_seconds = (clock.now() - start).seconds();
                RCLCPP_INFO(this->get_logger(), "go_to_velocity %s (%.3lf %.3lf %.3lf) time (%.3lfms)", 
                    key.c_str(), vel_target.x(), vel_target.y(), vel_target.z(), duration_seconds * 1000.0);

                vel_msg.header.stamp = clock.now();
                vel_msg.vel.x = vel_target.x();
                vel_msg.vel.y = vel_target.y();
                vel_msg.vel.z = vel_target.z();

                // check the difference in heading
                // Eigen::Vector3d rpy = 
                //     agent.transform.eulerAngles(2,1,0).reverse();
                vel_msg.yaw = 0.0;
                
                auto it = agents_comm.find(key);
                if (it != agents_comm.end())
                    it->second.vel_world_publisher->publish(vel_msg);
                break;
            }

            default:
                break;
            
        }

        std::string str_copy = key;
        // Remove cf from cfXX
        str_copy.erase(0,2);
        int id = std::stoi(str_copy);

        Marker target;
        target.header.frame_id = "/world";
        target.header.stamp = clock.now();
        target.type = visualization_msgs::msg::Marker::LINE_STRIP;
        target.id = id;
        target.action = visualization_msgs::msg::Marker::ADD;
        target.pose.orientation.x = 0.0;
        target.pose.orientation.y = 0.0;
        target.pose.orientation.z = 0.0;
        target.pose.orientation.w = 1.0;
        target.scale.x = 0.005;
        target.color.r = 0.5;
        target.color.g = 0.5;
        target.color.b = 1.0;
        target.color.a = 1.0;

        if(!agent.target_queue.empty())
        {
            // copy to prevent deleting the main target queue
            std::queue<Eigen::Vector3d> target_copy = agent.target_queue;

            Point p;
            p.x = agent.transform.translation().x();
            p.y = agent.transform.translation().y();
            p.z = agent.transform.translation().z();
            target.points.push_back(p);

            while (!target_copy.empty())
            {
                Point p;
                p.x = target_copy.front().x();
                p.y = target_copy.front().y();
                p.z = target_copy.front().z();
                target.points.push_back(p);

                target_copy.pop();
            }
        }

        AgentState agentstate;
        agentstate.id = key;
        agentstate.flight_state = agent.flight_state;
        agentstate.connected = agent.radio_connection;
        agentstate.completed = agent.completed;
        agentstate.mission_capable = agent.mission_capable;

        agents_feedback.agents.push_back(agentstate);
        target_array.markers.push_back(target);
    }

    // publish the flight state message
    agents_feedback.header.stamp = clock.now();
    agent_state_publisher->publish(agents_feedback);

    // publish the target data
    target_publisher->publish(target_array);
}