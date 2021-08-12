#pragma once

#include "depthai/depthai.hpp"
#include <thread>
#include <camera_info_manager/camera_info_manager.hpp>
#include "sensor_msgs/msg/camera_info.hpp"
#include "rclcpp/rclcpp.hpp"

namespace dai::rosBridge {

template <class RosMsg, class SimMsg> 
class BridgePublisher {
public:
  using ConvertFunc = std::function<void (std::shared_ptr<SimMsg> , RosMsg&)>;

  BridgePublisher(
      std::shared_ptr<dai::DataOutputQueue> daiMessageQueue, std::shared_ptr<rclcpp::Node> node,
      std::string rosTopic, ConvertFunc converter, rclcpp::QoS qosSetting, std::string cameraParamUri, std::string cameraName)
      : _daiMessageQueue(daiMessageQueue), _node(node), _converter(converter),
        _rosTopic(rosTopic){

      _rosPublisher = _node->create_publisher<RosMsg>(_rosTopic, qosSetting);
      if(!cameraParamUri.empty() && !cameraName.empty()){
          _isImageMessage = true;
          _camInfoManager = std::make_unique<camera_info_manager::CameraInfoManager>(_node.get(), cameraName, cameraParamUri);
          _cameraInfoTopic = cameraName + "/camera_info";
          _cameraInfoPublisher = _node->create_publisher<sensor_msgs::msg::CameraInfo>(_cameraInfoTopic, qosSetting);
      }
  }

  BridgePublisher(const BridgePublisher& other){
      _daiMessageQueue = other._daiMessageQueue;
      _node = other._node;
      _converter = other._converter;
      _rosTopic = other._rosTopic;
      _rosPublisher = other._rosPublisher;

      if(other._isImageMessage){
          _isImageMessage = true;
          _camInfoManager = std::make_unique<camera_info_manager::CameraInfoManager>(std::move(other._camInfoManager));
          _cameraInfoPublisher = other._cameraInfoPublisher;
      }
  }

  void addPubisherCallback()
  {
      _daiMessageQueue->addCallback(std::bind(&BridgePublisher<RosMsg, SimMsg>::daiCallback, this, std::placeholders::_1, std::placeholders::_2));
      _isCallbackAdded = true;
  }

  void publishHelper(std::shared_ptr<SimMsg> inDataPtr)
  {

      RosMsg opMsg;
      if (_camInfoFrameId.empty()){
          _converter(inDataPtr, opMsg);
          _camInfoFrameId = opMsg.header.frame_id;
      }

      if(_node->count_subscribers(_rosTopic) > 0){
          // std::cout << "before  " << opMsg.height << " " << opMsg.width << " " << opMsg.data.size() << std::endl;
          _converter(inDataPtr, opMsg);
          // std::cout << opMsg.height << " " << opMsg.width << " " << opMsg.data.size() << std::endl;
          _rosPublisher->publish(opMsg);

          if(_isImageMessage && _node->count_subscribers(_cameraInfoTopic) > 0){
              auto localCameraInfo = _camInfoManager->getCameraInfo();
              localCameraInfo.header.stamp = opMsg.header.stamp;
              localCameraInfo.header.frame_id = opMsg.header.frame_id;
              _cameraInfoPublisher->publish(localCameraInfo);
          }
      }

      if(_isImageMessage && _node->count_subscribers(_rosTopic) == 0 && _node->count_subscribers(_cameraInfoTopic) > 0){
          _converter(inDataPtr, opMsg);
          auto localCameraInfo = _camInfoManager->getCameraInfo();
          localCameraInfo.header.stamp = opMsg.header.stamp;
          localCameraInfo.header.frame_id = _camInfoFrameId;
          _cameraInfoPublisher->publish(localCameraInfo);
      }
  }

  void startPublisherThread()
  {
      if(_isCallbackAdded){
          std::runtime_error("addPubisherCallback() function adds a callback to the"
                             "depthai which handles the publishing so no need to start"
                             "the thread using startPublisherThread() ");
      }

      _readingThread = std::thread([&](){
        while(rclcpp::ok()){
            // auto daiDataPtr = _daiMessageQueue->get<SimMsg>();
            auto daiDataPtr = _daiMessageQueue->tryGet<SimMsg>();

            if(daiDataPtr == nullptr) {
                //  std::cout << "No data found!!!" <<std::endl;
                continue;
            }
            publishHelper(daiDataPtr);

        }
      });
  }

  ~BridgePublisher()
  {
      _readingThread.join();
  }
  
private:
  /** 
   * adding this callback will allow you to still be able to consume 
   * the data for other processing using get() function .
   */
  void daiCallback(std::string name, std::shared_ptr<ADatatype> data)
  {
      // std::cout << "In callback " << name << std::endl;
      auto daiDataPtr = std::dynamic_pointer_cast<SimMsg>(data);
      publishHelper(daiDataPtr);
  }
  
  std::shared_ptr<dai::DataOutputQueue> _daiMessageQueue;
  ConvertFunc _converter;
  
  std::shared_ptr<rclcpp::Node> _node;
  rclcpp::Publisher<sensor_msgs::msg::CameraInfo>::SharedPtr _cameraInfoPublisher;
  std::thread _readingThread;
  std::string _rosTopic, _camInfoFrameId, _cameraInfoTopic;
  std::unique_ptr<camera_info_manager::CameraInfoManager> _camInfoManager;
  bool _isCallbackAdded = false;
  bool _isImageMessage = false; // used to enable camera info manager
  typename rclcpp::Publisher<RosMsg>::SharedPtr _rosPublisher;
  
};


} // namespace dai::rosBridge
