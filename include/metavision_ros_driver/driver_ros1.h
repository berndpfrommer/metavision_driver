// -*-c++-*---------------------------------------------------------------------------------------
// Copyright 2021 Bernd Pfrommer <bernd.pfrommer@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef METAVISION_ROS_DRIVER__DRIVER_ROS1_H_
#define METAVISION_ROS_DRIVER__DRIVER_ROS1_H_

#include <camera_info_manager/camera_info_manager.h>
#include <dvs_msgs/EventArray.h>
#include <dynamic_reconfigure/server.h>
#include <event_array2_msgs/EventArray2.h>
#include <metavision/sdk/driver/camera.h>
#include <prophesee_event_msgs/EventArray.h>
#include <ros/ros.h>
#include <std_srvs/Trigger.h>

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "metavision_ros_driver/MetaVisionDynConfig.h"
#include "metavision_ros_driver/metavision_wrapper.h"

namespace metavision_ros_driver
{
namespace ph = std::placeholders;
template <class MsgType>

class DriverROS1 : public CallbackHandler
{
public:
  using Config = MetaVisionDynConfig;
  explicit DriverROS1(const ros::NodeHandle & nh) : nh_(nh)
  {
    bool status = start();
    if (!status) {
      ROS_ERROR("startup failed!");
      throw std::runtime_error("startup of CameraDriver node failed!");
    }
  }
  ~DriverROS1()
  {
    stop();
    wrapper_.reset();  // invoke destructor
  }

  void setBias(int * current, const std::string & name)
  {
    wrapper_->setBias(name, *current);
    const int new_val = wrapper_->getBias(name);
    *current = new_val;  // feed back if not changed to desired value!
  }

  void configure(Config & config, int level)
  {
    if (level < 0) {  // initial call
      // initialize config from current settings
      config.bias_diff = wrapper_->getBias("bias_diff");
      config.bias_diff_off = wrapper_->getBias("bias_diff_off");
      config.bias_diff_on = wrapper_->getBias("bias_diff_on");
      config.bias_fo = wrapper_->getBias("bias_fo");
      config.bias_hpf = wrapper_->getBias("bias_hpf");
      config.bias_pr = wrapper_->getBias("bias_pr");
      config.bias_refr = wrapper_->getBias("bias_refr");
      ROS_INFO("initialized config to camera biases");
    } else {
      setBias(&config.bias_diff, "bias_diff");
      setBias(&config.bias_diff_off, "bias_diff_off");
      setBias(&config.bias_diff_on, "bias_diff_on");
      setBias(&config.bias_fo, "bias_fo");
      setBias(&config.bias_hpf, "bias_hpf");
      setBias(&config.bias_pr, "bias_pr");
      setBias(&config.bias_refr, "bias_refr");
    }
    config_ = config;  // remember current values
  }

  bool start()
  {
    cameraInfoURL_ = nh_.param<std::string>("camerainfo_url", "");
    frameId_ = nh_.param<std::string>("frame_id", "");
    const double mtt = nh_.param<double>("message_time_threshold", 100e-6);
    messageTimeThreshold_ = ros::Duration(mtt);
    reserveSize_ = (size_t)(nh_.param<double>("sensors_max_mevs", 50.0) / std::max(mtt, 1e-6));

    pub_ = nh_.advertise<MsgType>("events", nh_.param<int>("send_queue_size", 1000));

    wrapper_ = std::make_shared<MetavisionWrapper>();

    if (!wrapper_->initialize(
          nh_.param<bool>("use_multithreading", false),
          nh_.param<double>("statistics_print_interval", 1.0),
          nh_.param<std::string>("bias_file", ""))) {
      ROS_ERROR("driver initialization failed!");
      return (false);
    }
    width_ = wrapper_->getWidth();
    height_ = wrapper_->getHeight();
    if (frameId_.empty()) {
      // default frame id to last 4 digits of serial number
      const auto sn = wrapper_->getSerialNumber();
      frameId_ = sn.substr(sn.size() - 4);
    }
    wrapper_->startCamera(this);
    ROS_INFO_STREAM("using frame id: " << frameId_);

    infoManager_ = std::make_shared<camera_info_manager::CameraInfoManager>(nh_, cameraInfoURL_);
    cameraInfoMsg_ = infoManager_->getCameraInfo();
    cameraInfoMsg_.header.frame_id = frameId_;

    // hook up dynamic config server *after* the camera has
    // been initialized so we can read the bias values
    configServer_.reset(new dynamic_reconfigure::Server<Config>(nh_));
    configServer_->setCallback(boost::bind(&DriverROS1::configure, this, _1, _2));

    saveBiasService_ = nh_.advertiseService("save_biases", &DriverROS1::saveBiases, this);

    ROS_INFO_STREAM("driver initialized successfully.");
    return (true);
  }

  void publish(const Metavision::EventCD * start, const Metavision::EventCD * end) override
  {
    if (t0_ == 0) {
      t0_ = ros::Time::now().toNSec();
    }
    if (pub_.getNumSubscribers() == 0) {
      return;
    }
    if (!msg_) {  // must allocate new message
      msg_.reset(new MsgType());
      msg_->header.frame_id = frameId_;
      msg_->header.seq = seq_++;
      msg_->width = width_;
      msg_->height = height_;
      msg_->events.reserve(reserveSize_);
    }
    const size_t n = end - start;
    auto & events = msg_->events;
    const size_t old_size = events.size();
    // The resize should not trigger a
    // copy with proper reserved capacity.
    events.resize(events.size() + n);
    // copy data into ROS message. For the SilkyEvCam
    // the full load packet size delivered by the SDK is 320
    int eventCount[2] = {0, 0};
    for (unsigned int i = 0; i < n; i++) {
      const auto & e_src = start[i];
      auto & e_trg = events[i + old_size];
      e_trg.x = e_src.x;
      e_trg.y = e_src.y;
      e_trg.polarity = e_src.p;
      e_trg.ts.fromNSec(t0_ + e_src.t * 1e3);
      eventCount[e_src.p]++;
    }
    wrapper_->updateEventCount(0, eventCount[0]);
    wrapper_->updateEventCount(1, eventCount[1]);
    const ros::Time & t_msg = msg_->events.begin()->ts;
    const ros::Time & t_last = msg_->events.rbegin()->ts;
    if (t_last > t_msg + messageTimeThreshold_) {
      msg_->header.stamp = t_msg;
      pub_.publish(msg_);
      wrapper_->updateEventsSent(events.size());
      wrapper_->updateMsgsSent(1);
      msg_.reset();  // no longer using this one
    }
  }

  bool keepRunning() override { return (ros::ok()); }

private:
  bool stop()
  {
    if (wrapper_) {
      return (wrapper_->stop());
    }
    return (false);
  }

  bool saveBiases(std_srvs::Trigger::Request & req, std_srvs::Trigger::Response & res)
  {
    (void)req;
    res.success = false;
    if (wrapper_) {
      res.success = wrapper_->saveBiases();
    }
    res.message += (res.success ? "succeeded" : "failed");
    return (res.success);
  }

  // ------------ variables
  ros::NodeHandle nh_;
  std::shared_ptr<MetavisionWrapper> wrapper_;
  std::shared_ptr<camera_info_manager::CameraInfoManager> infoManager_;
  ros::ServiceServer saveBiasService_;
  ros::Publisher pub_;
  std::shared_ptr<dynamic_reconfigure::Server<Config>> configServer_;
  Config config_;

  sensor_msgs::CameraInfo cameraInfoMsg_;
  boost::shared_ptr<MsgType> msg_;

  std::string cameraInfoURL_;
  ros::Duration messageTimeThreshold_;  // duration for triggering a message
  size_t reserveSize_{0};               // how many events to preallocate per message
  uint64_t t0_{0};                      // time base
  int width_;                           // image width
  int height_;                          // image height
  std::string frameId_;                 // ROS frame id
  uint64_t seq_{0};                     // ROS sequence number
};

template <>
void DriverROS1<event_array2_msgs::EventArray2>::publish(
  const Metavision::EventCD * start, const Metavision::EventCD * end)
{
  if (t0_ == 0) {
    t0_ = ros::Time::now().toNSec();
  }
  if (pub_.getNumSubscribers() == 0) {
    return;
  }
  if (!msg_) {  // must allocate new message
    msg_.reset(new event_array2_msgs::EventArray2());
    msg_->header.frame_id = frameId_;
    msg_->header.seq = static_cast<uint32_t>(seq_++);
    msg_->width = width_;
    msg_->height = height_;
    msg_->time_base = t0_ + (uint64_t)(start->t * 1e3);
    msg_->header.stamp.fromNSec(msg_->time_base);
    msg_->p_y_x_t.reserve(reserveSize_);
    msg_->seq = seq_;  // duplicate, but wanted symmetry with ROS2
  }
  const size_t n = end - start;
  const size_t old_size = msg_->p_y_x_t.size();
  // The resize should not trigger a
  // copy with proper reserved capacity.
  msg_->p_y_x_t.resize(old_size + n);
  // copy data into ROS message. For the SilkyEvCam
  // the full load packet size delivered by the SDK is n = 320
  int eventCount[2] = {0, 0};
  uint64_t * pyxt = &(msg_->p_y_x_t[old_size]);
  const uint64_t t_base = msg_->time_base;
  for (unsigned int i = 0; i < n; i++) {
    const auto & e = start[i];
    const uint64_t ts = t0_ + (uint64_t)(e.t * 1e3);
    const uint64_t dt = (ts - t_base) & 0xFFFFFFFFULL;
    pyxt[i] = (uint64_t)e.p << 63 | (uint64_t)e.y << 48 | (uint64_t)e.x << 32 | dt;
    eventCount[e.p]++;
  }
  wrapper_->updateEventCount(0, eventCount[0]);
  wrapper_->updateEventCount(1, eventCount[1]);
  ros::Time t_last;
  t_last.fromNSec(t0_ + (uint64_t)(start[n - 1].t * 1e3));
  if (t_last > msg_->header.stamp + messageTimeThreshold_) {
    wrapper_->updateEventsSent(msg_->p_y_x_t.size());
    wrapper_->updateMsgsSent(1);
    pub_.publish(msg_);
    msg_.reset();  // no longer using this one
  }
}

}  // namespace metavision_ros_driver
#endif  // METAVISION_ROS_DRIVER__DRIVER_ROS1_H_
