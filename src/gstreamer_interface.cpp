#include <gstreamer_interface.hpp>


namespace depthai_ctrl
{
GstInterface::GstInterface(int argc, char * argv[])
: _mLoopContext(nullptr), _mLoopThread(nullptr), _mLoop(nullptr),
  _pipeline(nullptr), _appSource(nullptr), _encoderProfile("H264"),
  _busWatchId(0), _bus(nullptr), _needDataSignalId(0),
  _isStreamPlaying(false), _isStreamDefault(false), _encoderWidth(1280),
  _encoderHeight(720), _encoderFps(25), _encoderBitrate(3000000), _rtspSink(nullptr),
  _udpSink(nullptr), _queue1(nullptr), _testSrc(nullptr), _textOverlay(nullptr),
  _h26xEnc(nullptr), _testSrcFilter(nullptr), _h26xCamCapsFilter(nullptr),
  _h26xparse(nullptr), _h26xpay(nullptr), _h26xTestCapsFilter(nullptr),
  _segment(nullptr), _testSyncIdentity(nullptr), _camSyncIdentity(nullptr),
  _inputSelector(nullptr), _h26xTestparse(nullptr), _queue2(nullptr)
{
  _streamAddress = "";
  gst_init(&argc, &argv);
  g_mutex_init(&haveDataCondMutex);
  g_mutex_init(&startStreamingCondMutex);
  _mLoopContext = g_main_context_default();
  _mLoop = g_main_loop_new(_mLoopContext, false);
}

GstInterface::~GstInterface()
{
  GstFlowReturn ret;
  if (_needDataSignalId != 0) {
    g_signal_handler_disconnect(_appSource, _needDataSignalId);
  }
  if (_appSource != nullptr) {
    g_signal_emit_by_name(_appSource, "end-of-stream", &ret);
    if (ret != GST_FLOW_OK) {
      g_printerr("Error: Emit end-of-stream failed\n");
    }
  }
  if (_pipeline) {
    gst_element_set_state(_pipeline, GST_STATE_NULL);
    gst_object_unref(_pipeline);
    _pipeline = nullptr;
  }
  if (_bus) {
    gst_bus_remove_watch(_bus);
    gst_object_unref(_bus);
  }
  if (_busWatchId != 0) {
    g_source_remove(_busWatchId);
    _busWatchId = 0;
  }
  if (_mLoop) {
    g_main_loop_quit(_mLoop);
    _mLoop = nullptr;
  }
  if (_mLoopThread) {
    g_thread_join(_mLoopThread);
  }
  if (_mLoopContext) {
    g_main_context_unref(_mLoopContext);
  }
  _isStreamPlaying = false;
}

void GstInterface::StartStream(void)
{
  if (_isStreamPlaying) {
    return;
  }

  std::cout << "Start stream called!" << std::endl;


}
void GstInterface::StopStream(void)
{
  GstFlowReturn ret;

  if (_needDataSignalId != 0) {
    g_signal_handler_disconnect(_appSource, _needDataSignalId);
  }
  if (_appSource != nullptr) {
    g_signal_emit_by_name(_appSource, "end-of-stream", &ret);
    if (ret != GST_FLOW_OK) {
      g_printerr("Error: Emit end-of-stream failed\n");
    }
  }
  if (_pipeline != nullptr) {
    gst_element_set_state(_pipeline, GST_STATE_NULL);
  }
  if (_mLoop != nullptr) {
    g_main_loop_quit(_mLoop);
  }
  if (_mLoopThread != nullptr) {
    g_thread_join(_mLoopThread);
  }
  _isStreamPlaying = false;
}

void GstInterface::BuildDefaultPipeline()
{
  _isStreamDefault = true;
  const bool is_udp_protocol = (_streamAddress.find("udp://") == 0);
  _pipeline = gst_pipeline_new("default_pipeline");

  // Video test source.
  _testSrc = gst_element_factory_make("videotestsrc", "source");
  g_object_set(G_OBJECT(_testSrc), "pattern", 2, NULL);
  _testSrcFilter = gst_element_factory_make("capsfilter", "source_filter");
  g_object_set(
    G_OBJECT(_testSrcFilter), "caps",
    gst_caps_new_simple(
      "video/x-raw",
      "format", G_TYPE_STRING, "I420",
      "width", G_TYPE_INT, _encoderWidth,
      "height", G_TYPE_INT, _encoderHeight,
      "framerate", GST_TYPE_FRACTION, _encoderFps, 1,
      NULL), NULL);

  // Text overlay.
  _textOverlay = gst_element_factory_make("textoverlay", "text");
  g_object_set(
    G_OBJECT(_textOverlay),
    "text", "Camera not detected!",
    "valignment", 4,             // 4 = center
    "halignment", 1,             // 1 = center
    "font-desc", "Sans, 42",
    NULL);

  // Software encoder and parser.
  if (_encoderProfile == "H265") {
    _h26xEnc = gst_element_factory_make("x265enc", "encoder");
    g_object_set(
      G_OBJECT(_h26xEnc),
      "bitrate", 500,               // 500 kbit/sec
      "speed-preset", 2,               // 2 = superfast
      "tune", 4,               // 4 = zerolatency
      NULL);
    _h26xparse = gst_element_factory_make("h265parse", "parser");
  } else {
    _h26xEnc = gst_element_factory_make("x264enc", "encoder");
    _h26xparse = gst_element_factory_make("h264parse", "parser");
  }

  // Sink element. UDP or RTSP client.
  if (is_udp_protocol) {
    // UDP Sink
    if (_encoderProfile == "H265") {
      _h26xpay = gst_element_factory_make("rtph265pay", "payload");
    } else {
      _h26xpay = gst_element_factory_make("rtph264pay", "payload");
    }
    g_object_set(G_OBJECT(_h26xpay), "pt", 96, NULL);
    _udpSink = gst_element_factory_make("udpsink", "udp_sink");
    g_object_set(
      G_OBJECT(_udpSink), "host", DepthAIUtils::ReadIpFromUdpAddress(
        _streamAddress).c_str(), NULL);
    g_object_set(
      G_OBJECT(_udpSink), "port", DepthAIUtils::ReadPortFromUdpAddress(
        _streamAddress), NULL);
  } else {
    // RTSP client sink
    _rtspSink = gst_element_factory_make("rtspclientsink", "rtsp_sink");
    g_object_set(
      G_OBJECT(_rtspSink),
      "protocols", 4,               // 4 = tcp
      "tls-validation-flags", 0,
      "location", _streamAddress.c_str(),
      NULL);
  }

  // Caps definition for source element.
  std::string profile = _encoderProfile;
  std::stringstream ss;
  std::string gstFormat;
  std::transform(profile.begin(), profile.end(), profile.begin(), ::tolower);
  ss << "video/x-" << profile;
  ss >> gstFormat;
  _h26xTestCapsFilter = gst_element_factory_make("capsfilter", "encoder_filter");
  g_object_set(
    G_OBJECT(_h26xTestCapsFilter), "caps",
    gst_caps_new_simple(
      gstFormat.c_str(),
      "profile", G_TYPE_STRING, "baseline",
      "pass", G_TYPE_INT, 5,
      "trellis", G_TYPE_BOOLEAN, false,
      "tune", G_TYPE_STRING, "zero-latency",
      "threads", G_TYPE_INT, 0,
      "speed-preset", G_TYPE_STRING, "superfast",
      "subme", G_TYPE_INT, 1,
      "bitrate", G_TYPE_INT, 4000,
      NULL), NULL);
  g_assert(_pipeline);

  if (is_udp_protocol) {
    gst_bin_add_many(
      GST_BIN(
        _pipeline), _testSrc, _testSrcFilter, _textOverlay, _h26xEnc, _h26xTestCapsFilter, _h26xparse, _h26xpay, _udpSink,
      NULL);
    gst_element_link_many(
      _testSrc, _testSrcFilter, _textOverlay, _h26xEnc, _h26xTestCapsFilter,
      _h26xparse, _h26xpay, _udpSink, NULL);
  } else {
    gst_bin_add_many(
      GST_BIN(
        _pipeline), _testSrc, _testSrcFilter, _textOverlay, _h26xEnc, _h26xTestCapsFilter, _h26xparse, _rtspSink,
      NULL);
    gst_element_link_many(
      _testSrc, _testSrcFilter, _textOverlay, _h26xEnc, _h26xTestCapsFilter,
      _h26xparse, _rtspSink, NULL);
  }
  gst_debug_bin_to_dot_file(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline");

}

gboolean GstInterface::SwitchSources(GstElement * video_switch)
{
  gint nb_sources;
  GstPad *active_pad, *new_pad;
  gchar *active_name;

  g_message ("switching");
  g_object_get (G_OBJECT (video_switch), "n-pads", &nb_sources, NULL);
  g_object_get (G_OBJECT (video_switch), "active-pad", &active_pad, NULL);

  active_name = gst_pad_get_name (active_pad);
  if (strcmp (active_name, "sink_0") == 0) {
    new_pad = gst_element_get_static_pad (video_switch, "sink_1");
  } else {
    new_pad = gst_element_get_static_pad (video_switch, "sink_0");
  }
  g_object_set (G_OBJECT (video_switch), "active-pad", new_pad, NULL);
  g_free (active_name);
  gst_object_unref (new_pad);

  g_message ("current number of sources : %d, active source %s",
      nb_sources, gst_pad_get_name (active_pad));

  return (GST_STATE (GST_ELEMENT (video_switch)) == GST_STATE_PLAYING);
}

void GstInterface::BuildPipeline()
{
  const bool is_udp_protocol = (_streamAddress.find("udp://") == 0);
  const std::string h26xparse = (_encoderProfile == "H264") ? "h264parse" : "h265parse";
  const std::string h26xencoder = (_encoderProfile == "H264") ? "x264enc" : "x265enc";
  const std::string gstFormat = (_encoderProfile == "H264") ? "video/x-h264" : "video/x-h265";

  /*auto end_time = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  g_mutex_lock(&haveDataCondMutex);
  while (queue.empty()) {
    std::cout << "Queue is empty!" << std::endl;
    if (!g_cond_wait_until(&haveDataCond, &haveDataCondMutex, end_time))
    {
      std::cout << "Queue is empty and 5s timeout! Default pipeline will be used." << std::endl;
      //g_mutex_unlock(&data->haveDataCondMutex);
      break;
    }
  }
  g_mutex_unlock(&haveDataCondMutex);*/
  // DISABLE CHECK FOR NOW
  if (queue.empty() and false) {     // video-data is not available - use "default" video output
    BuildDefaultPipeline();
  } else {
    _isStreamDefault = false;

    _pipeline = gst_pipeline_new("rgbCamSink_pipeline");

    // Source element for the camera not found stream.

    // Video test source.
    _testSrc = gst_element_factory_make("videotestsrc", "videotestsrc");
    g_object_set(G_OBJECT(_testSrc), "pattern", 16, NULL);
    _testSrcFilter = gst_element_factory_make("capsfilter", "source_filter");
    g_object_set(
      G_OBJECT(_testSrcFilter), "caps",
      gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "I420",
        "width", G_TYPE_INT, _encoderWidth,
        "height", G_TYPE_INT, _encoderHeight,
        "framerate", GST_TYPE_FRACTION, _encoderFps, 1,
        NULL), NULL);


    // Text overlay.
    _textOverlay = gst_element_factory_make("textoverlay", "text");
    g_object_set(
      G_OBJECT(_textOverlay),
      "text", "Camera not detected!",
      "valignment", 4,           // 4 = center
      "halignment", 1,           // 1 = center
      "font-desc", "Sans, 42",
      NULL);

    // Software encoder and parser.
    if (_encoderProfile == "H265") {
      _h26xEnc = gst_element_factory_make("x265enc", "encoder");
      g_object_set(
        G_OBJECT(_h26xEnc),
        "bitrate", 500,             // 500 kbit/sec
        "speed-preset", 2,             // 2 = superfast
        "tune", 4,             // 4 = zerolatency
        NULL);
    } else {
      _h26xEnc = gst_element_factory_make("x264enc", "encoder");
    }

    // Caps definition for source element.
    std::string profile = _encoderProfile;
    std::stringstream ss;
    std::string gstFormat;
    std::transform(profile.begin(), profile.end(), profile.begin(), ::tolower);
    ss << "video/x-" << profile;
    ss >> gstFormat;

    _h26xTestCapsFilter = gst_element_factory_make("capsfilter", "test_encoder_filter");
    g_object_set(
      G_OBJECT(_h26xTestCapsFilter), "caps",
      gst_caps_new_simple(
        gstFormat.c_str(),
        "profile", G_TYPE_STRING, "baseline",
        "pass", G_TYPE_INT, 5,
        "trellis", G_TYPE_BOOLEAN, false,
        "tune", G_TYPE_STRING, "zero-latency",
        "threads", G_TYPE_INT, 0,
        "speed-preset", G_TYPE_STRING, "superfast",
        "subme", G_TYPE_INT, 1,
        "bitrate", G_TYPE_INT, 4000,
        NULL), NULL);
    g_assert(_pipeline);

    // Source element for the camera.
    _appSource = gst_element_factory_make("appsrc", "appsource");
    g_object_set(
      G_OBJECT(_appSource),
      "do-timestamp", true,
      "is-live", true,
      "block", false,
      "stream-type", 0,
      NULL);
    gst_util_set_object_arg(G_OBJECT(_appSource), "format", "GST_FORMAT_TIME");
    // H26x parser. Is this really needed?
    if (_encoderProfile == "H265") {
      _h26xTestparse = gst_element_factory_make("h265parse", "test_parser");
    } else {
      _h26xTestparse = gst_element_factory_make("h264parse", "test_parser");
    }
    _queue2 = gst_element_factory_make("queue", "queue2");
    
    if (_encoderProfile == "H265") {
      _h26xparse = gst_element_factory_make("h265parse", "parser");
    } else {
      _h26xparse = gst_element_factory_make("h264parse", "parser");
    }
    _queue1 = gst_element_factory_make("queue", "queue1");
    // Sink element. UDP or RTSP client.
    if (is_udp_protocol) {
      // UDP Sink
      if (_encoderProfile == "H265") {
        _h26xpay = gst_element_factory_make("rtph265pay", "payload");
      } else {
        _h26xpay = gst_element_factory_make("rtph264pay", "payload");
      }
      g_object_set(G_OBJECT(_h26xpay), "pt", 96, NULL);
      _udpSink = gst_element_factory_make("udpsink", "udp_sink");
      g_object_set(
        G_OBJECT(_udpSink), "host", DepthAIUtils::ReadIpFromUdpAddress(
          _streamAddress).c_str(), NULL);
      g_object_set(
        G_OBJECT(_udpSink), "port", DepthAIUtils::ReadPortFromUdpAddress(
          _streamAddress), NULL);
    } else {
      // RTSP client sink
      _rtspSink = gst_element_factory_make("rtspclientsink", "rtsp_sink");
      g_object_set(
        G_OBJECT(_rtspSink),
        "protocols", 4,             // 4 = tcp
        "tls-validation-flags", 0,
        "location", _streamAddress.c_str(),
        NULL);
    }

    g_object_set(
      G_OBJECT(_appSource), "caps",
      gst_caps_new_simple(
        gstFormat.c_str(),
        "width", G_TYPE_INT, _encoderWidth,
        "height", G_TYPE_INT, _encoderHeight,
        "framerate", GST_TYPE_FRACTION, _encoderFps, 1,
        NULL), NULL);

    _h26xCamCapsFilter = gst_element_factory_make("capsfilter", "cam_encoder_filter");
    g_object_set(
      G_OBJECT(_h26xCamCapsFilter), "caps",
      gst_caps_new_simple(
        gstFormat.c_str(),
        "profile", G_TYPE_STRING, "main",
        "stream-format", G_TYPE_STRING, "byte-stream",
        NULL), NULL);


    _inputSelector = gst_element_factory_make("input-selector", "video_switch");

    _segment = gst_element_factory_make("identity", "identity-segment");
    g_object_set(G_OBJECT(_segment), "silent", TRUE, NULL);
    //g_signal_connect (G_OBJECT (segment), "notify::last-message",
    //G_CALLBACK (last_message_received), segment);

    g_object_set(G_OBJECT(_segment), "single-segment", TRUE, NULL);

    _testSyncIdentity = gst_element_factory_make("identity", "test_src");
    g_object_set(G_OBJECT(_testSyncIdentity), "sync", FALSE, NULL);
    _camSyncIdentity = gst_element_factory_make("identity", "cam_src");
    g_object_set(G_OBJECT(_camSyncIdentity), "sync", FALSE, NULL);
    if (is_udp_protocol) {
      gst_bin_add_many(
        GST_BIN(
          _pipeline), _appSource, _h26xCamCapsFilter, _camSyncIdentity, _h26xparse, _queue1, _h26xpay, _udpSink,
        NULL);
      gst_bin_add_many(
        GST_BIN(
          _pipeline), _testSrc, _testSrcFilter, _textOverlay, _h26xEnc, _h26xTestCapsFilter,
        _testSyncIdentity, _segment, _inputSelector, NULL);

      gst_element_link(_appSource, _h26xCamCapsFilter);
      gst_element_link(_h26xCamCapsFilter, _camSyncIdentity);
      gst_element_link(_camSyncIdentity, _inputSelector);

      gst_element_link(_testSrc, _testSrcFilter);
      gst_element_link(_testSrcFilter, _textOverlay);
      gst_element_link(_textOverlay, _h26xEnc);
      gst_element_link(_h26xEnc, _h26xTestCapsFilter);
      gst_element_link(_h26xTestCapsFilter, _testSyncIdentity);
      gst_element_link(_testSyncIdentity, _inputSelector);

      gst_element_link(_inputSelector, _segment);
      gst_element_link(_segment, _h26xparse);
      gst_element_link(_h26xparse, _queue1);
      gst_element_link(_queue1, _h26xpay);
      gst_element_link(_h26xpay, _udpSink);

    } else {
      gst_bin_add_many(
        GST_BIN(
          _pipeline), _appSource, _h26xCamCapsFilter, _camSyncIdentity, _h26xparse, _queue1, _rtspSink,
        _testSrc, _testSrcFilter, _textOverlay, _h26xEnc, _h26xTestCapsFilter,
        _testSyncIdentity, _segment, _inputSelector, NULL);

      
      gst_element_link(_appSource, _h26xCamCapsFilter);
      gst_element_link(_h26xCamCapsFilter, _camSyncIdentity);
      gst_element_link(_camSyncIdentity, _inputSelector);

      gst_element_link(_testSrc, _testSrcFilter);
      gst_element_link(_testSrcFilter, _textOverlay);
      gst_element_link(_textOverlay, _h26xEnc);
      gst_element_link(_h26xEnc, _h26xTestCapsFilter);
      gst_element_link(_h26xTestCapsFilter, _testSyncIdentity);
      gst_element_link(_testSyncIdentity, _inputSelector);

      gst_element_link(_inputSelector, _segment);
      gst_element_link(_segment, _h26xparse);
      gst_element_link(_h26xparse, _queue1);
      gst_element_link(_queue1, _rtspSink);
    }

    gst_debug_bin_to_dot_file(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline_begin");

    //g_timeout_add (20000, (GSourceFunc) GstInterface::SwitchSources, _inputSelector);
  }

  _bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline));
  _busWatchId = gst_bus_add_watch(_bus, GstInterface::StreamEventCallBack, this);
  gst_object_unref(_bus);
  _bus = nullptr;

  _mLoopThread = g_thread_new("GstThread", (GThreadFunc)GstInterface::PlayStream, this);


  _needDataSignalId =
    g_signal_connect(_appSource, "need-data", G_CALLBACK(GstInterface::NeedDataCallBack), this);

  // _needDataSignalId =
  // g_signal_connect(_appSource, "need-data", G_CALLBACK(NeedDataCallBack), this);


}

void GstInterface::NeedDataCallBack(
  GstElement * appsrc, guint unused_size,
  gpointer user_data)
{
  GstInterface * data = (GstInterface *)user_data;
  GstFlowReturn result;
/*
  while (data->queue.empty()) {
    std::cout << "Need data called with empty queue! Sleeping 10ms" << std::endl;
std::this_thread::sleep_for(std::chrono::milliseconds(10));
    //g_usleep(10000000); // 10ms
  }*/
  //std::cout << "Need data called!" << std::endl;

  gint64 end_time;  
  end_time = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
  if (!data->_isStreamDefault) {
    g_mutex_lock(&data->haveDataCondMutex);
    while (data->queue.empty()) {
      //std::cout << "Queue is empty!" << std::endl;

      if (!g_cond_wait_until (&data->haveDataCond, &data->haveDataCondMutex, end_time)){
            
        std::cout << "Timeout passed. Switching to default stream!" << std::endl;
        GstInterface::SwitchSources(data->_inputSelector);
        g_mutex_unlock (&data->haveDataCondMutex);
        return;
      }
      //g_cond_wait(&data->haveDataCond, &data->haveDataCondMutex);
    }
    auto videoPtr = data->queue.front();
    data->queueMutex.lock();
    data->queue.pop();
    data->queueMutex.unlock();
    g_mutex_unlock(&data->haveDataCondMutex);

    auto & frame = videoPtr->data;
    GstBuffer * buffer = gst_buffer_new_and_alloc(frame.size());
    gst_buffer_fill(buffer, 0, &frame[0], frame.size());
    const auto stamp = videoPtr->header.stamp;
    const GstClockTime gst_stamp = stamp.sec * 1000000000UL + stamp.nanosec;

    if (data->_stamp0 == 0) {
      data->_stamp0 = gst_stamp;
    }

    const GstClockTime local_stamp = gst_stamp - data->_stamp0;
    GST_BUFFER_PTS(buffer) = local_stamp;

    g_signal_emit_by_name(appsrc, "push-buffer", buffer, &result);
    gst_buffer_unref(buffer);
    //const auto result = ::gst_app_src_push_buffer(GST_APP_SRC(data->_appSource), buffer);
    /*std::cout << "Submitted: " << gst_stamp << " local=" << local_stamp << ": " << std::to_string(
      result) <<
      std::endl;*/
  }
}

void * GstInterface::PlayStream(gpointer data)
{
  GstInterface * gstImpl = (GstInterface *)data;
  std::cout << "PlayStream callback called." << std::endl;
  // DepthAICam * depthAICam = depthAIGst->depthAICam;
  // if (depthAICam != nullptr) {
  //     depthAICam->StartStreaming();
  // }
/*
  while (gstImpl->queue.empty()) {
    std::cout << "PlayStream called with empty queue! Sleeping 10ms" << std::endl;
std::this_thread::sleep_for(std::chrono::milliseconds(10));
    //g_usleep(10000000); // 10ms
  }*/
  std::cout << "GStreamer(PlayStream): Pipeline Change State Playing" << std::endl;

  gst_element_set_state(gstImpl->_pipeline, GST_STATE_PLAYING);
  gstImpl->_isStreamPlaying = true;
  g_main_loop_run(gstImpl->_mLoop);
  g_thread_exit(gstImpl->_mLoopThread);

  return nullptr;
}

gboolean GstInterface::StreamPlayingRestartCallback(gpointer user_data)
{
  GstInterface * gstImpl = (GstInterface *)user_data;

  g_debug("Restart stream because of connection failed.\n");
  if (gstImpl->_appSource) {
    gstImpl->_needDataSignalId = g_signal_connect(
      gstImpl->_appSource, "need-data",
      G_CALLBACK(GstInterface::NeedDataCallBack), gstImpl);
  }
  gst_element_set_state(gstImpl->_pipeline, GST_STATE_PLAYING);

  return G_SOURCE_REMOVE;
}

// Use this function to execute code when timer is removed.
void GstInterface::StreamPlayingRestartDone(gpointer user_data)
{
  (void)user_data;
}
} //namespace depthai_ctrl
