#include "depthai_ctrl/depthai_camera.hpp"
#include "depthai_ctrl/depthai_utils.h"
#include <nlohmann/json.hpp>

using namespace depthai_ctrl;

using std::placeholders::_1;
using std::placeholders::_2;
using Profile = dai::VideoEncoderProperties::Profile;

using std::chrono::duration_cast;
using std::chrono::nanoseconds;
using std::chrono::seconds;
static const std::vector<std::string> labelMap = {
  "person", "bicycle", "car", "motorbike", "aeroplane", "bus", "train", "truck", "boat",
  "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog",
  "horse",
  "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag",
  "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite", "baseball bat",
  "baseball glove",
  "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork", "knife",
  "spoon",
  "bowl", "banana", "apple", "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza",
  "donut", "cake", "chair", "sofa", "pottedplant", "bed", "diningtable", "toilet", "tvmonitor",
  "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
  "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"};

void DepthAICamera::Initialize()
{
  RCLCPP_INFO(get_logger(), "[%s]: Initializing...", get_name());
  /*declare_parameter<std::string>("left_camera_topic", "camera/left/image_raw");
  declare_parameter<std::string>("right_camera_topic", "camera/right/image_raw");
  declare_parameter<std::string>("color_camera_topic", "camera/color/image_raw");
  declare_parameter<std::string>("video_stream_topic", "camera/color/video");
  declare_parameter<std::string>("passthrough_topic", "camera/color/image_passthrough");
  declare_parameter<std::string>("stream_control_topic", "videostreamcmd");
  declare_parameter<std::string>("detection_roi_topic", "detections");*/
  declare_parameter<std::string>("nn_directory", "tiny-yolo-v4_openvino_2021.2_6shave.blob");
  declare_parameter<std::string>("camera_name", "oak");

  /*const std::string left_camera_topic = get_parameter("left_camera_topic").as_string();
  const std::string right_camera_topic = get_parameter("right_camera_topic").as_string();
  const std::string color_camera_topic = get_parameter("color_camera_topic").as_string();
  const std::string video_stream_topic = get_parameter("video_stream_topic").as_string();
  const std::string passthrough_topic = get_parameter("passthrough_topic").as_string();
  const std::string detection_roi_topic = get_parameter("detection_roi_topic").as_string();
  const std::string stream_control_topic = get_parameter("stream_control_topic").as_string();*/
  _nn_directory = get_parameter("nn_directory").as_string();

  _left_publisher = create_publisher<ImageMsg>("~/left/image_raw", 10);
  _right_publisher = create_publisher<ImageMsg>("~/right/image_raw", 10);
  _color_publisher = create_publisher<ImageMsg>("~/color/image_raw", 10);
  _passthrough_publisher = create_publisher<ImageMsg>("~/color/image_passthrough", 10);

  _detection_roi_publisher = create_publisher<vision_msgs::msg::Detection2DArray>(
    "~/detections", 10);

  _video_publisher = create_publisher<CompressedImageMsg>(
    "~/color/video", 10);

  _stream_command_subscriber = create_subscription<std_msgs::msg::String>(
    "~/videostreamcmd", 10,
    std::bind(&DepthAICamera::VideoStreamCommand, this, _1));

  // Video Stream parameters
  declare_parameter<std::string>("encoding", "H264");
  declare_parameter<int>("width", 1280);
  declare_parameter<int>("height", 720);
  declare_parameter<int>("fps", 25);
  declare_parameter<int>("bitrate", 3000000);
  declare_parameter<int>("lens_position", 120);
  declare_parameter<bool>("use_mono_cams", false);
  declare_parameter<bool>("use_raw_color_cam", false);
  declare_parameter<bool>("use_video_from_color_cam", true);
  declare_parameter<bool>("use_auto_focus", false);
  declare_parameter<bool>("use_usb_three", false);
  declare_parameter<bool>("use_neural_network", false);
  declare_parameter<bool>("use_passthrough_preview", false);

  _videoWidth = get_parameter("width").as_int();
  _videoHeight = get_parameter("height").as_int();
  _videoFps = get_parameter("fps").as_int();
  _videoBitrate = get_parameter("bitrate").as_int();
  _videoLensPosition = get_parameter("lens_position").as_int();
  _videoH265 = (get_parameter("encoding").as_string() == "H265");
  _useMonoCams = get_parameter("use_mono_cams").as_bool();
  _useRawColorCam = get_parameter("use_raw_color_cam").as_bool();
  _useVideoFromColorCam = get_parameter("use_video_from_color_cam").as_bool();
  _useAutoFocus = get_parameter("use_auto_focus").as_bool();
  _useNeuralNetwork = get_parameter("use_neural_network").as_bool();
  _syncNN = get_parameter("use_passthrough_preview").as_bool();
  _cameraName = get_parameter("camera_name").as_string();
  _left_camera_frame = _cameraName + "_left_camera_optical_frame";
  _right_camera_frame = _cameraName + "_right_camera_optical_frame";
  _color_camera_frame = _cameraName + "_rgb_camera_optical_frame";

  if (_useNeuralNetwork) {
    RCLCPP_INFO(
      get_logger(), "[%s]: Using neural network, blob path %s",
      get_name(), _nn_directory.c_str());
  }

  // USB2 can only handle one H264 stream from camera. Adding raw camera or mono cameras will
  // cause dropped messages and unstable latencies between frames. When using USB3, we can
  // support multiple streams without any bandwidth issues.
  _useUSB3 = get_parameter("use_usb_three").as_bool();
  _lastFrameTime = get_clock()->now();
}


void DepthAICamera::VideoStreamCommand(std_msgs::msg::String::SharedPtr msg)
{
  nlohmann::json cmd{};
  try {
    cmd = nlohmann::json::parse(msg->data.c_str());
  } catch (...) {
    RCLCPP_ERROR(this->get_logger(), "Error while parsing JSON string from VideoCommand");
    return;
  }
  if (!cmd["Command"].empty()) {
    std::string command = cmd["Command"];
    std::transform(
      command.begin(), command.end(), command.begin(),
      [](unsigned char c) {return std::tolower(c);});
    if (command == "start" && !_thread_running) {
      int width = _videoWidth;
      int height = _videoHeight;
      int fps = _videoFps;
      int bitrate = _videoBitrate;

      int videoLensPosition = _videoLensPosition;
      std::string encoding = _videoH265 ? "H265" : "H264";
      std::string error_message{};
      bool useMonoCams = _useMonoCams;
      bool useRawColorCam = _useRawColorCam;
      bool useAutoFocus = _useAutoFocus;


      if (!cmd["Width"].empty() && cmd["Width"].is_number_integer()) {
        nlohmann::from_json(cmd["Width"], width);
      }
      if (!cmd["Height"].empty() && cmd["Height"].is_number_integer()) {
        nlohmann::from_json(cmd["Height"], height);
      }
      if (!cmd["Fps"].empty() && cmd["Fps"].is_number_integer()) {
        nlohmann::from_json(cmd["Fps"], fps);
      }
      if (!cmd["Bitrate"].empty() && cmd["Bitrate"].is_number_integer()) {
        nlohmann::from_json(cmd["Bitrate"], bitrate);
      }
      if (!cmd["Encoding"].empty() && cmd["Encoding"].is_string()) {
        nlohmann::from_json(cmd["Encoding"], encoding);
      }
      if (!cmd["UseMonoCams"].empty() && cmd["UseMonoCams"].is_string()) {
        nlohmann::from_json(cmd["UseMonoCams"], useMonoCams);
      }
      if (!cmd["UseAutoFocus"].empty() && cmd["UseAutoFocus"].is_boolean()) {
        nlohmann::from_json(cmd["UseAutoFocus"], useAutoFocus);
      }
      if (!cmd["LensPosition"].empty() && cmd["LensPosition"].is_number_integer()) {
        nlohmann::from_json(cmd["LensPosition"], videoLensPosition);
      }

      if (DepthAIUtils::ValidateCameraParameters(
          width, height, fps, bitrate, videoLensPosition, encoding,
          error_message))
      {
        _videoWidth = width;
        _videoHeight = height;
        _videoFps = fps;
        _videoBitrate = bitrate;
        _videoLensPosition = videoLensPosition;
        _videoH265 = (encoding == "H265");
        _useMonoCams = useMonoCams;
        _useRawColorCam = useRawColorCam;
        _useAutoFocus = useAutoFocus;

        TryRestarting();
      } else {
        RCLCPP_ERROR(this->get_logger(), error_message.c_str());
      }
    }
    if (command == "change_focus" && _thread_running) {
      bool useAutoFocus = _useAutoFocus;
      if (!cmd["UseAutoFocus"].empty() && cmd["UseAutoFocus"].is_boolean()) {
        nlohmann::from_json(cmd["UseAutoFocus"], useAutoFocus);
      }
      if (_useAutoFocus != useAutoFocus) {
        _useAutoFocus = useAutoFocus;
        changeFocusMode(_useAutoFocus);
        RCLCPP_INFO(
          this->get_logger(), "Change focus mode to %s",
          _useAutoFocus ? "auto" : "manual");
      }
      if (useAutoFocus) {
        RCLCPP_ERROR(this->get_logger(), "Cannot change focus while auto focus is enabled");
      } else {

        int videoLensPosition = get_parameter("lens_position").as_int();

        if (!cmd["LensPosition"].empty() && cmd["LensPosition"].is_number_integer()) {
          nlohmann::from_json(cmd["LensPosition"], videoLensPosition);
          RCLCPP_INFO(this->get_logger(), "Received lens position cmd of %d", videoLensPosition);
        }
        if (videoLensPosition >= 0 || videoLensPosition <= 255) {
          RCLCPP_INFO(this->get_logger(), "Changing focus to %d", videoLensPosition);
          _videoLensPosition = videoLensPosition;
          changeLensPosition(_videoLensPosition);
        } else {
          RCLCPP_ERROR(
            this->get_logger(), "Required video stream 'lens_position' is incorrect.\
            Valid range is 0-255");
        }
      }
    }
  }
}

void DepthAICamera::TryRestarting()
{
  if (_thread_running) {
    _thread_running = false;
  }

  RCLCPP_INFO(this->get_logger(), "[%s]: (Re)Starting...", get_name());

  _pipeline = std::make_shared<dai::Pipeline>();

  // Using mono cameras adds additional CPU consumption, therefore it is disabled by default
  if (_useMonoCams) {
    auto monoLeft = _pipeline->create<dai::node::MonoCamera>();
    auto monoRight = _pipeline->create<dai::node::MonoCamera>();
    auto xoutLeft = _pipeline->create<dai::node::XLinkOut>();
    auto xoutRight = _pipeline->create<dai::node::XLinkOut>();
    // Setup Grayscale Cameras
    monoLeft->setResolution(dai::MonoCameraProperties::SensorResolution::THE_720_P);
    monoLeft->setBoardSocket(dai::CameraBoardSocket::LEFT);
    monoRight->setResolution(dai::MonoCameraProperties::SensorResolution::THE_720_P);
    monoRight->setBoardSocket(dai::CameraBoardSocket::RIGHT);
    monoLeft->out.link(xoutLeft->input);
    monoRight->out.link(xoutRight->input);
    xoutLeft->setStreamName("left");
    xoutRight->setStreamName("right");
  }
  auto colorCamera = _pipeline->create<dai::node::ColorCamera>();
  auto videoEncoder = _pipeline->create<dai::node::VideoEncoder>();
  auto xoutVideo = _pipeline->create<dai::node::XLinkOut>();
  xoutVideo->setStreamName("enc26xColor");
  // Setup Color Camera
  colorCamera->setBoardSocket(dai::CameraBoardSocket::RGB);
  colorCamera->setResolution(dai::ColorCameraProperties::SensorResolution::THE_1080_P);

  // Preview resolution cannot be larger than Video's, thus resolution color camera image is limited
  if (_useNeuralNetwork) {
    colorCamera->setPreviewSize(416, 416);
  } else {
    colorCamera->setPreviewSize(_videoWidth, _videoHeight);
  }
  colorCamera->setVideoSize(_videoWidth, _videoHeight);
  colorCamera->setFps(_videoFps);

  // Like mono cameras, color camera is disabled by default to reduce computational load.
  auto xoutColor = _pipeline->create<dai::node::XLinkOut>();
  xoutColor->setStreamName("color");
  if (_useRawColorCam) {
    if (_useVideoFromColorCam) {
      xoutColor->input.setBlocking(false);
      xoutColor->input.setQueueSize(1);
      colorCamera->video.link(xoutColor->input);
    } else if (!_useNeuralNetwork) {
      colorCamera->preview.link(xoutColor->input);
    } else {
      RCLCPP_WARN(
        this->get_logger(), "Color camera video is disabled because neural network is enabled");
    }
  }

  auto nnOut = _pipeline->create<dai::node::XLinkOut>();
  auto nnPassthroughOut = _pipeline->create<dai::node::XLinkOut>();
  nnOut->setStreamName("detections");
  nnPassthroughOut->setStreamName("pass");
  if (_useNeuralNetwork) {
    auto detectionNetwork = _pipeline->create<dai::node::YoloDetectionNetwork>();

    colorCamera->setPreviewKeepAspectRatio(false);
    colorCamera->setInterleaved(false);
    colorCamera->setColorOrder(dai::ColorCameraProperties::ColorOrder::BGR);
    // Network specific settings
    detectionNetwork->setConfidenceThreshold(0.5f);
    detectionNetwork->setNumClasses(80);
    detectionNetwork->setCoordinateSize(4);
    detectionNetwork->setAnchors({10, 14, 23, 27, 37, 58, 81, 82, 135, 169, 344, 319});
    detectionNetwork->setAnchorMasks({{"side26", {1, 2, 3}}, {"side13", {3, 4, 5}}});
    detectionNetwork->setIouThreshold(0.5f);
    detectionNetwork->setBlobPath(_nn_directory);
    detectionNetwork->setNumInferenceThreads(2);

    detectionNetwork->input.setBlocking(false);

    // Linking
    colorCamera->preview.link(detectionNetwork->input);
    if (_syncNN) {
      detectionNetwork->passthrough.link(nnPassthroughOut->input);
    }
    detectionNetwork->out.link(nnOut->input);
  }
  Profile encoding = _videoH265 ? Profile::H265_MAIN : Profile::H264_MAIN;
  videoEncoder->setDefaultProfilePreset(_videoFps, encoding);
  videoEncoder->setBitrate(_videoBitrate);
  RCLCPP_INFO(
    this->get_logger(), "[%s]: VideoEncoder FPS: %f",
    get_name(), videoEncoder->getFrameRate());

  colorCamera->video.link(videoEncoder->input);
  videoEncoder->bitstream.link(xoutVideo->input);
  auto xinColor = _pipeline->create<dai::node::XLinkIn>();
  xinColor->setStreamName("colorCamCtrl");

  xinColor->out.link(colorCamera->inputControl);
  RCLCPP_INFO(this->get_logger(), "[%s]: Initializing DepthAI camera...", get_name());
  for (int i = 0; i < 5 && !_device; i++) {
    try {
      _device = std::make_shared<dai::Device>(*_pipeline, !_useUSB3);
    } catch (const std::runtime_error & err) {
      RCLCPP_ERROR(get_logger(), "Cannot start DepthAI camera: %s", err.what());
      _device.reset();
    }
  }
  if (!_device) {
    return;
  }

  _calibrationHandler = _device->readCalibration();

  dai::rosBridge::ImageConverter rgbConverter(_color_camera_frame, true);
  sensor_msgs::msg::CameraInfo rgbCameraInfo = rgbConverter.calibrationToCameraInfo(
    _calibrationHandler, dai::CameraBoardSocket::RGB, _videoWidth, _videoHeight);
  // print camerainfo
  RCLCPP_INFO(this->get_logger(), "[%s]: CameraInfo:", get_name());
  RCLCPP_INFO(this->get_logger(), "[%s]:   width: %d", get_name(), rgbCameraInfo.width);
  RCLCPP_INFO(this->get_logger(), "[%s]:   height: %d", get_name(), rgbCameraInfo.height);
  RCLCPP_INFO(
    this->get_logger(), "[%s]:   distortion_model: %s",
    get_name(), rgbCameraInfo.distortion_model.c_str());
  RCLCPP_INFO(
    this->get_logger(), "[%s]:   D: [%f, %f, %f, %f, %f]",
    get_name(), rgbCameraInfo.d[0], rgbCameraInfo.d[1], rgbCameraInfo.d[2], rgbCameraInfo.d[3],
    rgbCameraInfo.d[4]);
  RCLCPP_INFO(
    this->get_logger(), "[%s]:   K: [%f, %f, %f, %f, %f, %f, %f, %f, %f]",
    get_name(), rgbCameraInfo.k[0], rgbCameraInfo.k[1], rgbCameraInfo.k[2], rgbCameraInfo.k[3],
    rgbCameraInfo.k[4], rgbCameraInfo.k[5], rgbCameraInfo.k[6], rgbCameraInfo.k[7],
    rgbCameraInfo.k[8]);
  RCLCPP_INFO(
    this->get_logger(), "[%s]:   R: [%f, %f, %f, %f, %f, %f, %f, %f, %f]",
    get_name(), rgbCameraInfo.r[0], rgbCameraInfo.r[1], rgbCameraInfo.r[2], rgbCameraInfo.r[3],
    rgbCameraInfo.r[4], rgbCameraInfo.r[5], rgbCameraInfo.r[6], rgbCameraInfo.r[7],
    rgbCameraInfo.r[8]);
  RCLCPP_INFO(
    this->get_logger(), "[%s]:   P: [%f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f]",
    get_name(), rgbCameraInfo.p[0], rgbCameraInfo.p[1], rgbCameraInfo.p[2], rgbCameraInfo.p[3],
    rgbCameraInfo.p[4], rgbCameraInfo.p[5], rgbCameraInfo.p[6], rgbCameraInfo.p[7],
    rgbCameraInfo.p[8], rgbCameraInfo.p[9], rgbCameraInfo.p[10], rgbCameraInfo.p[11]);

  std::string usbSpeed;
  switch (_device->getUsbSpeed()) {
    case dai::UsbSpeed::UNKNOWN:
      usbSpeed = "Unknown";
      break;
    case dai::UsbSpeed::LOW:
      usbSpeed = "Low";
      break;
    case dai::UsbSpeed::FULL:
      usbSpeed = "Full";
      break;
    case dai::UsbSpeed::HIGH:
      usbSpeed = "High";
      break;
    case dai::UsbSpeed::SUPER:
      usbSpeed = "Super";
      break;
    case dai::UsbSpeed::SUPER_PLUS:
      usbSpeed = "SuperPlus";
      break;
    default:
      usbSpeed = "Not valid";
      break;
  }
  RCLCPP_INFO(
    this->get_logger(), "[%s]: DepthAI Camera USB Speed: %s", get_name(),
    usbSpeed.c_str());
  //_device->startPipeline();
  _colorCamInputQueue = _device->getInputQueue("colorCamCtrl");
  dai::CameraControl colorCamCtrl;
  if (_useAutoFocus) {
    colorCamCtrl.setAutoFocusMode(dai::RawCameraControl::AutoFocusMode::CONTINUOUS_VIDEO);
  } else {
    colorCamCtrl.setAutoFocusMode(dai::RawCameraControl::AutoFocusMode::OFF);
    colorCamCtrl.setManualFocus(_videoLensPosition);
  }

  _colorCamInputQueue->send(colorCamCtrl);
  //dai::rosBridge::ImageConverter rgbConverter(_color_camera_frame, false);
  if (_useNeuralNetwork) {
    _neural_network_converter = std::make_shared<dai::rosBridge::ImgDetectionConverter>(
      _color_camera_frame, _videoWidth, _videoHeight, false);
    _neuralNetworkOutputQueue = _device->getOutputQueue("detections", 30, false);
    _neuralNetworkCallback =
      _neuralNetworkOutputQueue->addCallback(
      std::bind(
        &DepthAICamera::onNeuralNetworkCallback, this,
        std::placeholders::_1));
    if (_syncNN) {
      _passthroughQueue =
        _device->getOutputQueue("pass", 30, false);
      _passthroughCallback =
        _passthroughQueue->addCallback(
        std::bind(
          &DepthAICamera::onPassthroughCallback, this,
          std::placeholders::_1));
    }
  }
  if (_useRawColorCam) {
    _color_camera_converter = std::make_shared<dai::rosBridge::ImageConverter>(
      _color_camera_frame, false);
    _colorQueue = _device->getOutputQueue("color", 30, false);
    _colorCamCallback =
      _colorQueue->addCallback(
      std::bind(
        &DepthAICamera::onColorCamCallback, this,
        std::placeholders::_1));
  }
  _videoQueue = _device->getOutputQueue("enc26xColor", 30, true);
  if (_useMonoCams) {
    _left_camera_converter = std::make_shared<dai::rosBridge::ImageConverter>(
      _left_camera_frame, false);
    _right_camera_converter = std::make_shared<dai::rosBridge::ImageConverter>(
      _right_camera_frame,
      false);
    _leftQueue = _device->getOutputQueue("left", 30, false);
    _rightQueue = _device->getOutputQueue("right", 30, false);

    _leftCamCallback =
      _leftQueue->addCallback(
      std::bind(
        &DepthAICamera::onLeftCamCallback, this,
        std::placeholders::_1));
    _rightCamCallback =
      _rightQueue->addCallback(
      std::bind(
        &DepthAICamera::onRightCallback, this,
        std::placeholders::_1));
  }
  _thread_running = true;

  _videoEncoderCallback =
    _videoQueue->addCallback(
    std::bind(
      &DepthAICamera::onVideoEncoderCallback, this,
      std::placeholders::_1));

}

void DepthAICamera::changeLensPosition(int lens_position)
{
  if (!_device) {
    return;
  }
  dai::CameraControl colorCamCtrl;
  colorCamCtrl.setAutoFocusMode(dai::RawCameraControl::AutoFocusMode::OFF);
  colorCamCtrl.setManualFocus(lens_position);
  _colorCamInputQueue->send(colorCamCtrl);
}

void DepthAICamera::changeFocusMode(bool use_auto_focus)
{
  if (!_device) {
    return;
  }
  dai::CameraControl colorCamCtrl;
  if (use_auto_focus) {
    colorCamCtrl.setAutoFocusMode(dai::RawCameraControl::AutoFocusMode::CONTINUOUS_VIDEO);
  } else {
    colorCamCtrl.setAutoFocusMode(dai::RawCameraControl::AutoFocusMode::OFF);
    colorCamCtrl.setManualFocus(_videoLensPosition);
  }
  _colorCamInputQueue->send(colorCamCtrl);
}

void DepthAICamera::onLeftCamCallback(
  const std::shared_ptr<dai::ADatatype> data)
{
  (void)data; // Using this pointer does not pop from queue, so we don't need to do anything with it.
  std::vector<std::shared_ptr<dai::ImgFrame>> leftPtrVector =
    _leftQueue->tryGetAll<dai::ImgFrame>();
  RCLCPP_DEBUG(
    this->get_logger(), "[%s]: Received %ld left camera frames...",
    get_name(), leftPtrVector.size());
  for (std::shared_ptr<dai::ImgFrame> & leftPtr : leftPtrVector) {
    auto image = _left_camera_converter->toRosMsgPtr(leftPtr);
    _left_publisher->publish(*image);
  }

}

void DepthAICamera::onRightCallback(
  const std::shared_ptr<dai::ADatatype> data)
{
  (void)data;
  std::vector<std::shared_ptr<dai::ImgFrame>> rightPtrVector =
    _rightQueue->tryGetAll<dai::ImgFrame>();
  RCLCPP_DEBUG(
    this->get_logger(), "[%s]: Received %ld right camera frames...",
    get_name(), rightPtrVector.size());
  for (std::shared_ptr<dai::ImgFrame> & rightPtr : rightPtrVector) {
    auto image = _right_camera_converter->toRosMsgPtr(rightPtr);
    _right_publisher->publish(*image);
  }
}

void DepthAICamera::onColorCamCallback(
  const std::shared_ptr<dai::ADatatype> data)
{
  (void)data;
  std::vector<std::shared_ptr<dai::ImgFrame>> colorPtrVector =
    _colorQueue->tryGetAll<dai::ImgFrame>();
  RCLCPP_DEBUG(
    this->get_logger(), "[%s]: Received %ld color camera frames...",
    get_name(), colorPtrVector.size());
  for (std::shared_ptr<dai::ImgFrame> & colorPtr : colorPtrVector) {
    auto image = _color_camera_converter->toRosMsgPtr(colorPtr);
    _color_publisher->publish(*image);
  }
}

void DepthAICamera::onPassthroughCallback(
  const std::shared_ptr<dai::ADatatype> data)
{
  (void)data;
  std::vector<std::shared_ptr<dai::ImgFrame>> colorPtrVector =
    _passthroughQueue->tryGetAll<dai::ImgFrame>();
  RCLCPP_DEBUG(
    this->get_logger(), "[%s]: Received %ld color camera frames...",
    get_name(), colorPtrVector.size());
  for (std::shared_ptr<dai::ImgFrame> & colorPtr : colorPtrVector) {
    auto image = _passthrough_converter->toRosMsgPtr(colorPtr);
    _passthrough_publisher->publish(*image);
  }
}

void DepthAICamera::onVideoEncoderCallback(
  const std::shared_ptr<dai::ADatatype> data)
{
  (void)data;
  std::vector<std::shared_ptr<dai::ImgFrame>> videoPtrVector =
    _videoQueue->tryGetAll<dai::ImgFrame>();
  RCLCPP_DEBUG(
    this->get_logger(), "[%s]: Received %ld video frames...",
    get_name(), videoPtrVector.size());
  for (std::shared_ptr<dai::ImgFrame> & videoPtr : videoPtrVector) {

    /*
      Old implementation uses getTimestamp, which had a bug where the time is not correct when run at boot.
      getTimestamp is host syncronized and supposed to give the time in host clock.
      However, since the DepthAI camera is starting its boot at the same time as the host,
      The syncronization is not working properly as it tries to syncronize the camera clock with the host.
      Therefore, we use the getTimestampDevice() to get direct device time.
      This implementation will work without any problems for the H264 video streaming.
      However, a host syncronized time is needed for the raw color camera, when doing camera based navigation.
      Otherwise, the time drifts will cause wrong estimations and tracking will be unstable.

      It is also possible to use SequenceNumber for timestamp calculation, and it also works for H264 streaming.
      However, it might still be problematic with the raw color camera. It will be investigated later.
    */
    //const auto stamp = videoPtr->getTimestamp().time_since_epoch().count();
    const auto stamp = videoPtr->getTimestampDevice().time_since_epoch().count();
    //const auto seq = videoPtr->getSequenceNum();
    //int64_t stamp = (int64_t)seq * (1e9/_videoFps); // Use sequence number for timestamp

    CompressedImageMsg video_stream_chunk{};
    video_stream_chunk.header.frame_id = _color_camera_frame;

    // rclcpp::Time can be initialized directly with nanoseconds only.
    // Internally, when given with seconds and nanoseconds, it casts it to nanoseconds anyways.
    video_stream_chunk.header.stamp = rclcpp::Time(stamp, RCL_STEADY_TIME);
    video_stream_chunk.data.swap(videoPtr->getData());
    video_stream_chunk.format = _videoH265 ? "H265" : "H264";
    _video_publisher->publish(video_stream_chunk);
  }
}

void DepthAICamera::onNeuralNetworkCallback(
  const std::shared_ptr<dai::ADatatype> data)
{
  (void)data;
  std::vector<std::shared_ptr<dai::ImgDetections>> detectionsPtrVector =
    _neuralNetworkOutputQueue->tryGetAll<dai::ImgDetections>();
  for (std::shared_ptr<dai::ImgDetections> & detectionsPtr : detectionsPtrVector) {
    auto detections = _neural_network_converter->toRosMsgPtr(detectionsPtr);
    _detection_roi_publisher->publish(*detections);
  }
}

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(depthai_ctrl::DepthAICamera)
