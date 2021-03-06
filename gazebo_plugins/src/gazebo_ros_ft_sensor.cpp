/*
 *  Gazebo - Outdoor Multi-Robot Simulator
 *  Copyright (C) 2003  
 *     Nate Koenig & Andrew Howard
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
/*
 * Desc: Force Torque Sensor Plugin
 * Author: Francisco Suarez-Ruiz
 * Date: 5 June 2014
 */

#include <gazebo_plugins/gazebo_ros_ft_sensor.h>
#include <tf/tf.h>

namespace gazebo
{
GZ_REGISTER_MODEL_PLUGIN(GazeboRosFT);

////////////////////////////////////////////////////////////////////////////////
// Constructor
GazeboRosFT::GazeboRosFT()
{
  this->ft_connect_count_ = 0;
}

////////////////////////////////////////////////////////////////////////////////
// Destructor
GazeboRosFT::~GazeboRosFT()
{
  event::Events::DisconnectWorldUpdateBegin(this->update_connection_);
  // Custom Callback Queue
  this->queue_.clear();
  this->queue_.disable();
  this->rosnode_->shutdown();
  this->callback_queue_thread_.join();
  delete this->rosnode_;
}

////////////////////////////////////////////////////////////////////////////////
// Load the controller
void GazeboRosFT::Load( physics::ModelPtr _model, sdf::ElementPtr _sdf )
{
  // Save pointers
  this->model_ = _model;
  this->world_ = this->model_->GetWorld();
  
  // load parameters
  this->robot_namespace_ = "";
  if (_sdf->HasElement("robotNamespace"))
    this->robot_namespace_ = _sdf->GetElement("robotNamespace")->Get<std::string>() + "/";

  if (!_sdf->HasElement("jointName"))
  {
    ROS_FATAL("ft_sensor plugin missing <jointName>, cannot proceed");
    return;
  }
  else
    this->joint_name_ = _sdf->GetElement("jointName")->Get<std::string>();

  this->joint_ = this->model_->GetJoint(this->joint_name_);
  if (!this->joint_)
  {
    ROS_FATAL("gazebo_ros_ft_sensor plugin error: jointName: %s does not exist\n",this->joint_name_.c_str());
    return;
  }
  
  this->parent_link_ = this->joint_->GetParent();
  this->child_link_ = this->joint_->GetChild();
  this->frame_name_ = this->child_link_->GetName();
  
  ROS_INFO("ft_sensor plugin reporting wrench values to the frame [%s]", this->frame_name_.c_str());

  if (!_sdf->HasElement("topicName"))
  {
    ROS_FATAL("ft_sensor plugin missing <topicName>, cannot proceed");
    return;
  }
  else
    this->topic_name_ = _sdf->GetElement("topicName")->Get<std::string>();
  
  if (!_sdf->HasElement("updateRate"))
  {
    ROS_DEBUG("ft_sensor plugin missing <updateRate>, defaults to 0.0"
             " (as fast as possible)");
    this->update_rate_ = 0;
  }
  else
    this->update_rate_ = _sdf->GetElement("updateRate")->Get<double>();

  // Make sure the ROS node for Gazebo has already been initialized
  if (!ros::isInitialized())
  {
    ROS_FATAL_STREAM("A ROS node for Gazebo has not been initialized, unable to load plugin. "
      << "Load the Gazebo system plugin 'libgazebo_ros_api_plugin.so' in the gazebo_ros package)");
    return;
  }

  this->rosnode_ = new ros::NodeHandle(this->robot_namespace_);
  
  // resolve tf prefix
  std::string prefix;
  this->rosnode_->getParam(std::string("tf_prefix"), prefix);
  this->frame_name_ = tf::resolve(prefix, this->frame_name_);

  // Custom Callback Queue
  ros::AdvertiseOptions ao = ros::AdvertiseOptions::create<geometry_msgs::WrenchStamped>(
    this->topic_name_,1,
    boost::bind( &GazeboRosFT::FTConnect,this),
    boost::bind( &GazeboRosFT::FTDisconnect,this), ros::VoidPtr(), &this->queue_);
  this->pub_ = this->rosnode_->advertise(ao);
  
  // Custom Callback Queue
  this->callback_queue_thread_ = boost::thread( boost::bind( &GazeboRosFT::QueueThread,this ) );
  
  // New Mechanism for Updating every World Cycle
  // Listen to the update event. This event is broadcast every
  // simulation iteration.
  this->update_connection_ = event::Events::ConnectWorldUpdateBegin(
      boost::bind(&GazeboRosFT::UpdateChild, this));
}

////////////////////////////////////////////////////////////////////////////////
// Someone subscribes to me
void GazeboRosFT::FTConnect()
{
  this->ft_connect_count_++;
}

////////////////////////////////////////////////////////////////////////////////
// Someone subscribes to me
void GazeboRosFT::FTDisconnect()
{
  this->ft_connect_count_--;
}

////////////////////////////////////////////////////////////////////////////////
// Update the controller
void GazeboRosFT::UpdateChild()
{
  common::Time cur_time = this->world_->GetSimTime();

  // rate control
  if (this->update_rate_ > 0 &&
      (cur_time-this->last_time_).Double() < (1.0/this->update_rate_))
    return;
    
  if (this->ft_connect_count_ == 0)
    return;

  physics::JointWrench wrench;
  math::Vector3 torque;
  math::Vector3 force;

  // FIXME: Should include options for diferent frames and measure directions
  // E.g: https://bitbucket.org/osrf/gazebo/raw/default/gazebo/sensors/ForceTorqueSensor.hh
  // Get force torque at the joint
  // The wrench is reported in the CHILD <frame>
  // The <measure_direction> is child_to_parent
  wrench = this->joint_->GetForceTorque(0);
  force = wrench.body2Force;
  torque = wrench.body2Torque;


  this->lock_.lock();
  // copy data into wrench message
  this->wrench_msg_.header.frame_id = this->frame_name_;
  this->wrench_msg_.header.stamp.sec = (this->world_->GetSimTime()).sec;
  this->wrench_msg_.header.stamp.nsec = (this->world_->GetSimTime()).nsec;

  this->wrench_msg_.wrench.force.x    = force.x;
  this->wrench_msg_.wrench.force.y    = force.y;
  this->wrench_msg_.wrench.force.z    = force.z;
  this->wrench_msg_.wrench.torque.x   = torque.x;
  this->wrench_msg_.wrench.torque.y   = torque.y;
  this->wrench_msg_.wrench.torque.z   = torque.z;

  this->pub_.publish(this->wrench_msg_);
  this->lock_.unlock();
  
  // save last time stamp
  this->last_time_ = cur_time;
}

// Custom Callback Queue
////////////////////////////////////////////////////////////////////////////////
// custom callback queue thread
void GazeboRosFT::QueueThread()
{
  static const double timeout = 0.01;

  while (this->rosnode_->ok())
  {
    this->queue_.callAvailable(ros::WallDuration(timeout));
  }
}

}
