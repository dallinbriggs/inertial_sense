#include "inertial_sense.h"
#include <chrono>
#include <stddef.h>
#include <unistd.h>
#include <tf/tf.h>
#include <ros/console.h>

InertialSenseROS::InertialSenseROS() :
  nh_(), nh_private_("~"), initialized_(false)
{
  nh_private_.param<std::string>("port", port_, "/dev/ttyUSB0");
  nh_private_.param<int>("baudrate", baudrate_, 3000000);
  nh_private_.param<std::string>("frame_id", frame_id_, "body_inertial");

  /// Connect to the uINS

  memset(&serial_, 0, sizeof(serial_));
  serialPortPlatformInit(&serial_);
  ROS_INFO("Connecting to serial port \"%s\", at %d baud", port_.c_str(), baudrate_);
  if (serialPortOpen(&serial_, port_.c_str(), baudrate_, true) != 1)
  {
    ROS_FATAL("inertialsense: Unable to open serial port \"%s\", at %d baud", port_.c_str(), baudrate_);
    exit(0);
  }
  else
    ROS_INFO("Connected to uINS on \"%s\", at %d baud", port_.c_str(), baudrate_);
  
  // Initialize the IS parser
  comm_.buffer = message_buffer_;
  comm_.bufferSize = sizeof(message_buffer_);
  is_comm_init(&comm_);
  get_flash_config();

  // Make sure the navigation rate is right, if it's not, then we need to change and reset it.
  int nav_dt_ms;
  if (nh_private_.getParam("navigation_dt_ms", nav_dt_ms))
  {
    if (nav_dt_ms != flash_.startupNavDtMs)
    {
      int messageSize = is_comm_set_data(&comm_, DID_FLASH_CONFIG, offsetof(nvm_flash_cfg_t, startupNavDtMs), sizeof(uint32_t), &nav_dt_ms);
      serialPortWrite(&serial_, message_buffer_, messageSize);
      serialPortFlush(&serial_);
      ROS_INFO("navigation rate change from %dms to %dms, resetting uINS to make change", flash_.startupNavDtMs, nav_dt_ms);
      reset_device();
      
      // Re-request flash config to confirm change
      get_flash_config();
      if (flash_.startupNavDtMs != nav_dt_ms)
        ROS_ERROR("inertialsense: unable to change navigation rate from %dms to %dms", flash_.startupNavDtMs, nav_dt_ms);
      else
        ROS_INFO("Set navigation rate to %dms", flash_.startupNavDtMs);
    }
  }
  
  /// Start Up ROS service servers
  mag_cal_srv_ = nh_.advertiseService("single_axis_mag_cal", &InertialSenseROS::perform_mag_cal_srv_callback, this);
  multi_mag_cal_srv_ = nh_.advertiseService("multi_axis_mag_cal", &InertialSenseROS::perform_multi_mag_cal_srv_callback, this);
  
  // Stop all broadcasts
  uint32_t messageSize = is_comm_stop_broadcasts(&comm_);
  serialPortWrite(&serial_, message_buffer_, messageSize);

  /////////////////////////////////////////////////////////
  /// PARAMETER CONFIGURATION
  /////////////////////////////////////////////////////////
  set_vector_flash_config<float>("INS_rpy", 3, offsetof(nvm_flash_cfg_t, insRotation));
  set_vector_flash_config<float>("INS_xyz", 3, offsetof(nvm_flash_cfg_t, insOffset));
  set_vector_flash_config<float>("GPS_ant_xyz", 3, offsetof(nvm_flash_cfg_t, gps1AntOffset));
  set_vector_flash_config<double>("GPS_ref_lla", 3, offsetof(nvm_flash_cfg_t, refLla));
  
  set_flash_config<float>("inclination", offsetof(nvm_flash_cfg_t, magInclination), 1.14878541071f);  
  set_flash_config<float>("declination", offsetof(nvm_flash_cfg_t, magDeclination), 0.20007290992f);
  set_flash_config<int>("dynamic_model", offsetof(nvm_flash_cfg_t, insDynModel), 8);
  set_flash_config<int>("ser1_baud_rate", offsetof(nvm_flash_cfg_t, ser1BaudRate), 115200);


  /////////////////////////////////////////////////////////
  /// DATA STREAMS CONFIGURATION
  /////////////////////////////////////////////////////////
  
  uint32_t rmcBits = RMC_BITS_GPS_NAV | RMC_BITS_STROBE_IN_TIME; // we always need GPS for time synchronization
  nh_private_.param<bool>("stream_INS", INS_.enabled, true);
  if (INS_.enabled)
  {
    INS_.pub = nh_.advertise<nav_msgs::Odometry>("ins", 1);
    rmcBits |= RMC_BITS_DUAL_IMU | RMC_BITS_INS1 | RMC_BITS_INS2;
    
    // Request covariance information
    messageSize = is_comm_get_data(&comm_, DID_INL2_VARIANCE, 0, 0, nav_dt_ms);
    serialPortWrite(&serial_, message_buffer_, messageSize);
  }

  // Set up the IMU ROS stream
  nh_private_.param<bool>("stream_IMU", IMU_.enabled, false);
  if (IMU_.enabled)
  {
    IMU_.pub = nh_.advertise<sensor_msgs::Imu>("imu", 1);
//    IMU_.pub2 = nh_.advertise<sensor_msgs::Imu>("imu2", 1);
    rmcBits |= RMC_BITS_DUAL_IMU | RMC_BITS_INS1 | RMC_BITS_INS2;
  }

  // Set up the GPS ROS stream - we always need GPS information for time sync, just don't always need to publish it
  nh_private_.param<bool>("stream_GPS", GPS_.enabled, false);
  if (GPS_.enabled)
  {
    GPS_.pub = nh_.advertise<inertial_sense::GPS>("gps", 1);    
  }

  // Set up the GPS info ROS stream
  nh_private_.param<bool>("stream_GPS_info", GPS_info_.enabled, false);
  if (GPS_info_.enabled)
  {
    GPS_info_.pub = nh_.advertise<inertial_sense::GPSInfo>("gps/info", 1);
    rmcBits |= RMC_BITS_GPS1_SAT;
  }

  // Set up the magnetometer ROS stream
  nh_private_.param<bool>("stream_mag", mag_.enabled, false);
  if (mag_.enabled)
  {
    mag_.pub = nh_.advertise<sensor_msgs::MagneticField>("mag", 1);
//    mag_.pub2 = nh_.advertise<sensor_msgs::MagneticField>("mag2", 1);
    rmcBits |= RMC_BITS_MAGNETOMETER1;
  }

  // Set up the barometer ROS stream
  nh_private_.param<bool>("stream_baro", baro_.enabled, false);
  if (baro_.enabled)
  {
    baro_.pub = nh_.advertise<sensor_msgs::FluidPressure>("baro", 1);
    rmcBits |= RMC_BITS_BAROMETER;
  }

  // Set up the preintegrated IMU (coning and sculling integral) ROS stream
  nh_private_.param<bool>("stream_preint_IMU", dt_vel_.enabled, false);
  if (dt_vel_.enabled)
  {
    dt_vel_.pub = nh_.advertise<inertial_sense::PreIntIMU>("preint_imu", 1);
    rmcBits |= RMC_BITS_PREINTEGRATED_IMU;
  }
  
  messageSize = is_comm_get_data_rmc(&comm_, rmcBits);
  serialPortWrite(&serial_, message_buffer_, messageSize);
  
  /////////////////////////////////////////////////////////
  /// ASCII OUTPUT CONFIGURATION
  /////////////////////////////////////////////////////////
 
  int NMEA_rate = nh_private_.param<int>("NMEA_rate", 0);
  int NMEA_message_configuration = nh_private_.param<int>("NMEA_configuration", 0x00);
  int NMEA_message_ports = nh_private_.param<int>("NMEA_ports", 0x00);
  ascii_msgs_t msgs = {};
  msgs.options = (NMEA_message_ports & NMEA_SER0) ? RMC_OPTIONS_PORT_SER0 : 0; // output on serial 0
  msgs.options |= (NMEA_message_ports & NMEA_SER1) ? RMC_OPTIONS_PORT_SER1 : 0; // output on serial 1
  msgs.gpgga = (NMEA_message_configuration & NMEA_GPGGA) ? NMEA_rate : 0;
  msgs.gpgll = (NMEA_message_configuration & NMEA_GPGLL) ? NMEA_rate : 0;
  msgs.gpgsa = (NMEA_message_configuration & NMEA_GPGSA) ? NMEA_rate : 0;
  msgs.gprmc = (NMEA_message_configuration & NMEA_GPRMC) ? NMEA_rate : 0;
  messageSize = is_comm_set_data(&comm_, DID_ASCII_BCAST_PERIOD, 0, sizeof(ascii_msgs_t), &msgs);
  serialPortWrite(&serial_, message_buffer_, messageSize);  

  initialized_ = true;
}

template <typename T>
void InertialSenseROS::set_vector_flash_config(std::string param_name, uint32_t size, uint32_t offset){
  std::vector<double> tmp(size,0);
  T v[size];
  if (nh_private_.hasParam(param_name))
    nh_private_.getParam(param_name, tmp);
  for (int i = 0; i < size; i++)
  {
    v[i] = tmp[i];
  }
  
  int messageSize = is_comm_set_data(&comm_, DID_FLASH_CONFIG, offset, sizeof(v), v);
  serialPortWrite(&serial_, message_buffer_, messageSize);
}

template <typename T>
void InertialSenseROS::set_flash_config(std::string param_name, uint32_t offset, T def)
{
  T tmp;
  nh_private_.param<T>(param_name, tmp, def);
  int messageSize = is_comm_set_data(&comm_, DID_FLASH_CONFIG, offset, sizeof(T), &tmp);
  serialPortWrite(&serial_, message_buffer_, messageSize);
}

void InertialSenseROS::get_flash_config()
{
  got_flash_config = false;
  int messageSize = is_comm_get_data(&comm_, DID_FLASH_CONFIG, 0, 0, 0);
  serialPortWrite(&serial_, message_buffer_, messageSize);
  
  // wait for flash config message to confirm connection
  ros::Time start = ros::Time::now();
  while (!got_flash_config && (ros::Time::now() - start).toSec() <= 3.0)
    update();
  if ((ros::Time::now() - start).toSec() >= 3.0)
    ROS_FATAL("inertialsense: No response when requesting flash configuration from uINS on \"%s\", at %d baud", port_.c_str(), baudrate_);
}

void InertialSenseROS::flash_config_callback(const nvm_flash_cfg_t * const msg)
{
  got_flash_config = true;
  flash_ = (*msg);
}

void InertialSenseROS::INS1_callback(const ins_1_t * const msg)
{
  if (got_GPS_fix_ & inertial_init_)
  {
    std::vector<double> refLla_ (3);
    refLla_[0] = msg->lla[0];
    refLla_[1] = msg->lla[1];
    refLla_[2] = msg->lla[2];
    nh_private_.setParam("GPS_ref_lla", refLla_);
    set_vector_flash_config<double>("GPS_ref_lla", 3, offsetof(nvm_flash_cfg_t, refLla));
    inertial_init_ = false;
  }
  odom_msg.header.frame_id = frame_id_;

  odom_msg.pose.pose.position.x = msg->ned[0];
  odom_msg.pose.pose.position.y = msg->ned[1];
  odom_msg.pose.pose.position.z = msg->ned[2];
}

void InertialSenseROS::INS_variance_callback(const inl2_variance_t * const msg)
{
  // We have to convert NED velocity covariance into body-fixed
  tf::Matrix3x3 cov_vel_NED;
  cov_vel_NED.setValue(msg->PvelNED[0], 0, 0, 0, msg->PvelNED[1], 0, 0, 0, msg->PvelNED[2]);
  tf::Quaternion att;
  tf::quaternionMsgToTF(odom_msg.pose.pose.orientation, att);
  tf::Matrix3x3 R_NED_B(att);
  tf::Matrix3x3 cov_vel_B = R_NED_B.transposeTimes(cov_vel_NED * R_NED_B);

  // Populate Covariance Matrix
  for (int i = 0; i < 3; i++)
  {
    // Position and velocity covariance is only valid if in NAV mode (with GPS)
    if (insStatus_ & INS_STATUS_NAV_MODE)
    {
      odom_msg.pose.covariance[7*i] = msg->PxyzNED[i];
      for (int j = 0; j < 3; j++)
        odom_msg.twist.covariance[6*i+j] = cov_vel_B[i][j];
    }
    else
    {
      odom_msg.pose.covariance[7*i] = 0;
      odom_msg.twist.covariance[7*i] = 0;
    }
    odom_msg.pose.covariance[7*(i+3)] = msg->PattNED[i];
    odom_msg.twist.covariance[7*(i+3)] = msg->PWBias[i];
  }  
}


void InertialSenseROS::INS2_callback(const ins_2_t * const msg)
{
  insStatus_ = msg->insStatus;  
  odom_msg.header.stamp = ros::Time::now();
  odom_msg.header.frame_id = frame_id_;

  odom_msg.pose.pose.orientation.w = msg->qn2b[0];
  odom_msg.pose.pose.orientation.x = msg->qn2b[1];
  odom_msg.pose.pose.orientation.y = msg->qn2b[2];
  odom_msg.pose.pose.orientation.z = msg->qn2b[3];

  odom_msg.twist.twist.linear.x = msg->uvw[0];
  odom_msg.twist.twist.linear.y = msg->uvw[1];
  odom_msg.twist.twist.linear.z = msg->uvw[2];

  odom_msg.twist.twist.angular.x = imu1_msg.angular_velocity.x;
  odom_msg.twist.twist.angular.y = imu1_msg.angular_velocity.y;
  odom_msg.twist.twist.angular.z = imu1_msg.angular_velocity.z;
  if (INS_.enabled)
    INS_.pub.publish(odom_msg);
}


void InertialSenseROS::IMU_callback(const dual_imu_t* const msg)
{
  imu1_msg.header.stamp = ros::Time::now();
  imu1_msg.header.frame_id = imu2_msg.header.frame_id = frame_id_;

  imu1_msg.angular_velocity.x = msg->I[0].pqr[0];
  imu1_msg.angular_velocity.y = msg->I[0].pqr[1];
  imu1_msg.angular_velocity.z = msg->I[0].pqr[2];
  imu1_msg.linear_acceleration.x = msg->I[0].acc[0];
  imu1_msg.linear_acceleration.y = msg->I[0].acc[1];
  imu1_msg.linear_acceleration.z = msg->I[0].acc[2];

//  imu2_msg.angular_velocity.x = msg->I[1].pqr[0];
//  imu2_msg.angular_velocity.y = msg->I[1].pqr[1];
//  imu2_msg.angular_velocity.z = msg->I[1].pqr[2];
//  imu2_msg.linear_acceleration.x = msg->I[1].acc[0];
//  imu2_msg.linear_acceleration.y = msg->I[1].acc[1];
//  imu2_msg.linear_acceleration.z = msg->I[1].acc[2];

  if (IMU_.enabled)
  {
    IMU_.pub.publish(imu1_msg);
//    IMU_.pub2.publish(imu2_msg);
  }
}


void InertialSenseROS::GPS_callback(const gps_nav_t * const msg)
{
  GPS_week_ = msg->week;
  GPS_towOffset_ = msg->towOffset;
  if (GPS_.enabled)
  {
    gps_msg.header.stamp = ros::Time::now();
    gps_msg.fix_type = msg->status & GPS_STATUS_FIX_MASK;
    gps_msg.header.frame_id =frame_id_;
    gps_msg.num_sat = (uint8_t)(msg->status & GPS_STATUS_NUM_SATS_USED_MASK);
    gps_msg.cno = msg->cnoMean;
    gps_msg.latitude = msg->lla[0];
    gps_msg.longitude = msg->lla[1];
    gps_msg.altitude = msg->lla[2];
    gps_msg.hMSL = msg->hMSL;
    gps_msg.hAcc = msg->hAcc;
    gps_msg.vAcc = msg->vAcc;
    gps_msg.pDop = msg->pDop;
    gps_msg.linear_velocity.x = msg->velNed[0];
    gps_msg.linear_velocity.y = msg->velNed[1];
    gps_msg.linear_velocity.z = msg->velNed[2];
    GPS_.pub.publish(gps_msg);
  }

  if (!got_GPS_fix_)
  {
    if ((msg->status & 0xFF00) == (unsigned int)GPS_STATUS_FIX_3D)
    {
        got_GPS_fix_ = true;
    }
  }
}

void InertialSenseROS::update()
{
  uint8_t buffer[512];
  int bytes_read = serialPortReadTimeout(&serial_, buffer, 512, 1);

  for (int i = 0; i < bytes_read; i++)
  {
    uint32_t message_type = is_comm_parse(&comm_, buffer[i]);

    if (message_type == DID_FLASH_CONFIG)
    {
      flash_config_callback((nvm_flash_cfg_t*) message_buffer_);
      break;
    }

    if(initialized_)
    {
      switch (message_type)
      {
      case DID_NULL:
        // no valid message yet
        break;
      case DID_INS_1:
        INS1_callback((ins_1_t*) message_buffer_);
        break;
      case DID_INS_2:
        INS2_callback((ins_2_t*) message_buffer_);
        break;
      case DID_INL2_VARIANCE:
        INS_variance_callback((inl2_variance_t*) message_buffer_);
        break;

      case DID_DUAL_IMU:
        IMU_callback((dual_imu_t*) message_buffer_);
        break;

      case DID_GPS_NAV:
        GPS_callback((gps_nav_t*) message_buffer_);
        break;

      case DID_GPS1_SAT:
        GPS_Info_callback((gps_sat_t*) message_buffer_);
        break;

      case DID_MAGNETOMETER_1:
        mag_callback((magnetometer_t*) message_buffer_, 1);
        break;
      case DID_MAGNETOMETER_2:
        mag_callback((magnetometer_t*) message_buffer_, 2);
        break;

      case DID_BAROMETER:
        baro_callback((barometer_t*) message_buffer_);
        break;

      case DID_PREINTEGRATED_IMU:
        preint_IMU_callback((preintegrated_imu_t*) message_buffer_);
        break;
      case DID_STROBE_IN_TIME:
        strobe_in_time_callback((strobe_in_time_t*) message_buffer_);
        break;

      case -1:
        bad_data_callback(message_buffer_);
        break;

      default:
        ROS_INFO("Unhandled IS message %d", message_type);
        break;
      }
    }
  }
}

void InertialSenseROS::strobe_in_time_callback(const strobe_in_time_t * const msg)
{
  // create the subscriber if it doesn't exist
  if (strobe_pub_.getTopic().empty())
    strobe_pub_ = nh_.advertise<std_msgs::Header>("strobe_time", 1);
  
  std_msgs::Header strobe_msg;
  strobe_msg.stamp = ros_time_from_week_and_tow(msg->week, msg->timeOfWeekMs * 1e-3);
  strobe_pub_.publish(strobe_msg);  
}


void InertialSenseROS::GPS_Info_callback(const gps_sat_t* const msg)
{
  gps_info_msg.header.stamp =ros::Time::now();
  gps_info_msg.header.frame_id = frame_id_;
  gps_info_msg.num_sats = msg->numSats;
  for (int i = 0; i < 50; i++)
  {
    gps_info_msg.sattelite_info[i].sat_id = msg->sat[i].svId;
    gps_info_msg.sattelite_info[i].cno = msg->sat[i].cno;
  }
  GPS_info_.pub.publish(gps_info_msg);
}


void InertialSenseROS::mag_callback(const magnetometer_t* const msg, int mag_number)
{
  sensor_msgs::MagneticField mag_msg;
  mag_msg.header.stamp = ros::Time::now();
  mag_msg.header.frame_id = frame_id_;
  mag_msg.magnetic_field.x = msg->mag[0];
  mag_msg.magnetic_field.y = msg->mag[1];
  mag_msg.magnetic_field.z = msg->mag[2];
  
  if(mag_number == 1)
  {
    mag_.pub.publish(mag_msg);
  }
//  else
//  {
//    mag_.pub2.publish(mag_msg);
//  }
}

void InertialSenseROS::baro_callback(const barometer_t * const msg)
{
  sensor_msgs::FluidPressure baro_msg;
  baro_msg.header.stamp = ros::Time::now();
  baro_msg.header.frame_id = frame_id_;
  baro_msg.fluid_pressure = msg->bar;

  baro_.pub.publish(baro_msg);
}

void InertialSenseROS::preint_IMU_callback(const preintegrated_imu_t * const msg)
{
  inertial_sense::PreIntIMU preintIMU_msg;   
  preintIMU_msg.header.stamp = ros::Time::now();
  preintIMU_msg.header.frame_id = frame_id_;
  preintIMU_msg.dtheta.x = msg->theta1[0];
  preintIMU_msg.dtheta.y = msg->theta1[1];
  preintIMU_msg.dtheta.z = msg->theta1[2];

  preintIMU_msg.dvel.x = msg->vel1[0];
  preintIMU_msg.dvel.y = msg->vel1[1];
  preintIMU_msg.dvel.z = msg->vel1[2];

  preintIMU_msg.dt = msg->dt;

  dt_vel_.pub.publish(preintIMU_msg);
}

bool InertialSenseROS::perform_mag_cal_srv_callback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
  (void)req;
  res.success = true; 
  uint32_t single_axis_command = 1;
  int messageSize = is_comm_set_data(&comm_, DID_MAG_CAL, offsetof(mag_cal_t, enMagRecal), sizeof(uint32_t), &single_axis_command);
  serialPortWrite(&serial_, message_buffer_, messageSize);  
}

bool InertialSenseROS::perform_multi_mag_cal_srv_callback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
  (void)req;
  res.success = true; 
  uint32_t multi_axis_command = 0;
  int messageSize = is_comm_set_data(&comm_, DID_MAG_CAL, offsetof(mag_cal_t, enMagRecal), sizeof(uint32_t), &multi_axis_command);
  serialPortWrite(&serial_, message_buffer_, messageSize);  
}

void InertialSenseROS::reset_device()
{
  // send reset command
  uint32_t reset_command = 99;
  int messageSize = is_comm_set_data(&comm_, DID_CONFIG, offsetof(config_t, system), sizeof(uint32_t), &reset_command);
  serialPortWrite(&serial_, message_buffer_, messageSize);
  sleep(3);
}

void InertialSenseROS::bad_data_callback(const uint8_t *buf)
{
  std::cout << "\nbad data: " << std::endl;
  for (int i = 0; i < BUFFER_SIZE; i++)
  {
    printf("%x ", buf[i]);
  }
}

ros::Time InertialSenseROS::ros_time_from_week_and_tow(const uint32_t week, const double timeOfWeek)
{
  ros::Time rostime(0, 0);
  //  If we have a GPS fix, then use it to set timestamp
  if (GPS_towOffset_)
  {
    uint64_t sec = UNIX_TO_GPS_OFFSET + floor(timeOfWeek) + week*7*24*3600;
    uint64_t nsec = (timeOfWeek - floor(timeOfWeek))*1e9;
    rostime = ros::Time(sec, nsec);
  }
  else
  {
    // Otherwise, estimate the uINS boot time and offset the messages
    if (!got_first_message_)
    {
      got_first_message_ = true;
      INS_local_offset_ = ros::Time::now().toSec() - timeOfWeek;
    }
    else // low-pass filter offset to account for drift
    {
      double y_offset = ros::Time::now().toSec() - timeOfWeek;
      INS_local_offset_ = 0.005 * y_offset + 0.995 * INS_local_offset_;
    }
    // Publish with ROS time
    rostime = ros::Time(INS_local_offset_ + timeOfWeek);
  }
  return rostime;
}

ros::Time InertialSenseROS::ros_time_from_start_time(const double time)
{
  ros::Time rostime(0, 0);
  
  //  If we have a GPS fix, then use it to set timestamp
  if (GPS_towOffset_ > 0.001)
  {
    uint64_t sec = UNIX_TO_GPS_OFFSET + floor(time + GPS_towOffset_) + GPS_week_*7*24*3600;
    uint64_t nsec = (time + GPS_towOffset_ - floor(time + GPS_towOffset_))*1e9;
    rostime = ros::Time(sec, nsec);
  }
  else
  {
    // Otherwise, estimate the uINS boot time and offset the messages
    if (!got_first_message_)
    {
      got_first_message_ = true;
      INS_local_offset_ = ros::Time::now().toSec() - time;
    }
    else // low-pass filter offset to account for drift
    {
      double y_offset = ros::Time::now().toSec() - time;
      INS_local_offset_ = 0.005 * y_offset + 0.995 * INS_local_offset_;
    }
    // Publish with ROS time
    rostime = ros::Time(INS_local_offset_ + time);
  }
  return rostime;
}

ros::Time InertialSenseROS::ros_time_from_tow(const double tow)
{
  return ros_time_from_week_and_tow(GPS_week_, tow);
}


int main(int argc, char**argv)
 {
  ros::init(argc, argv, "inertial_sense_node");
  InertialSenseROS thing;
  while (ros::ok())
  {
    ros::spinOnce();
    thing.update();
  }
  return 0;
}
