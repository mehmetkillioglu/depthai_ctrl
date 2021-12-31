#include <depthai_ctrl/ImuConverter.hpp>

namespace dai
{

namespace ros
{

ImuConverter::ImuConverter(const std::string & frameName)
: _frameName(frameName), _sequenceNum(0) {}

void ImuConverter::toRosMsg(std::shared_ptr<dai::IMUData> inData, ImuMsgs::Imu & outImuMsg)
{
// setting the header
  outImuMsg.header.stamp = rclcpp::Clock().now();

  outImuMsg.header.frame_id = _frameName;

  const auto imuPacket = inData->packets[inData->packets.size() - 1];

  {
    const auto & rVvalues = imuPacket.rotationVector;

    outImuMsg.orientation.x = rVvalues.i;
    outImuMsg.orientation.y = rVvalues.j;
    outImuMsg.orientation.z = rVvalues.k;
    outImuMsg.orientation.w = rVvalues.real;
  }

  {
    const auto & gyroValues = imuPacket.gyroscope;

    outImuMsg.angular_velocity.x = gyroValues.x;
    outImuMsg.angular_velocity.y = gyroValues.y;
    outImuMsg.angular_velocity.z = gyroValues.z;
  }

  {
    const auto & acceleroValues = imuPacket.acceleroMeter;

    outImuMsg.linear_acceleration.x = acceleroValues.x;
    outImuMsg.linear_acceleration.y = acceleroValues.y;
    outImuMsg.linear_acceleration.z = acceleroValues.z;
  }

  _sequenceNum++;
}

ImuPtr ImuConverter::toRosMsgPtr(const std::shared_ptr<dai::IMUData> inData)
{
  ImuPtr ptr = std::make_shared<ImuMsgs::Imu>();

  toRosMsg(inData, *ptr);
  return ptr;
}

}  // namespace ros
}  // namespace dai
