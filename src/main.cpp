#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/core.hpp>
#include <opentok.h>
#include <cstdint>
#include <iostream>
#include "config.h"
#include "renderer.h"
#include "atomic"
#include <otk_thread.h>

using namespace cv;
typedef unsigned char byte;
long int begin_timestamp = 0;
struct timeval tp;
otc_session *session = nullptr;
extern int screen_cap;


static std::atomic<bool> g_is_connected(false);
static otc_publisher *g_publisher = nullptr;
static std::atomic<bool> g_is_publishing(false);
static std::map<std::string, void*> g_subscriber_map;
static std::mutex g_subscriber_map_mutex;

extern struct otc_audio_device_callbacks default_device_callbacks;
extern struct otc_session_callbacks default_session_callbacks;
extern struct otc_subscriber_callbacks default_subscriber_callbacks;
extern struct otc_publisher_callbacks default_publisher_callbacks;
extern struct otc_video_capturer_callbacks custom_capture_callbacks;

static void on_session_connected(otc_session *session, void *user_data)
{
  std::cout << __FUNCTION__ << " callback function" << std::endl;

  g_is_connected = true;

  if ((session != nullptr) && (g_publisher != nullptr))
  {
    if (otc_session_publish(session, g_publisher) == OTC_SUCCESS)
    {
      g_is_publishing = true;
      return;
    }
    std::cout << "Could not publish successfully" << std::endl;
  }
}


static void on_subscriber_render_frame(otc_subscriber *subscriber,
                                       void *user_data,
                                       const otc_video_frame *frame) {
  RendererManager *render_manager = static_cast<RendererManager*>(user_data);
  if (render_manager == nullptr) {
    return;
  }
  render_manager->addFrame(subscriber, frame);
}


static void on_session_stream_received(otc_session *session,
                                       void *user_data,
                                       const otc_stream *stream) {
  std::cout << __FUNCTION__ << " callback function" << std::endl;
  RendererManager *render_manager = static_cast<RendererManager*>(user_data);
  if (render_manager == nullptr) {
    return;
  }

  struct otc_subscriber_callbacks subscriber_callbacks = {0};
  subscriber_callbacks.user_data = user_data;

  subscriber_callbacks.on_render_frame = on_subscriber_render_frame;

  otc_subscriber *subscriber = otc_subscriber_new(stream,
                                                  &subscriber_callbacks);
                                              
  if (subscriber == nullptr) {
    std::cout << "Could not create OpenTok subscriber successfully" << std::endl;
    return;
  }
  render_manager->createNamedRenderer(subscriber, (void*)"Subscriber Window");
  if (otc_session_subscribe(session, subscriber) == OTC_SUCCESS) {
    const std::lock_guard<std::mutex> lock(g_subscriber_map_mutex);
    g_subscriber_map[std::string(otc_stream_get_id(stream))] = subscriber;
    return;
  }
  std::cout << "Could not subscribe successfully" << std::endl;
}

static void on_publisher_render_frame(otc_publisher *publisher,
                                      void *user_data,
                                      const otc_video_frame *frame) {
  RendererManager *render_manager = static_cast<RendererManager*>(user_data);  
  if (render_manager == nullptr) {
    std::cout << "Renderer is null" << std::endl;
    return;
  }
  render_manager->addFrame(publisher, frame);
}



static void on_otc_log_message(const char *message)
{
  std::cout << __FUNCTION__ << ":" << message << std::endl;
}

static void on_keypress_callback(const int32_t keypress)
{
  std::cout << __FUNCTION__ << ": Pressed key is " << keypress << std::endl;
  switch(keypress){
    case ('q'):
    {
      void * user_data = otc_publisher_get_user_data(g_publisher);
      RendererManager *render_manager = static_cast<RendererManager*>(user_data);       
      screen_cap = render_manager->quit_now(0); //end renderer
      std::cout << "ScreenCap " << screen_cap << std::endl;
      usleep(10000);
      otc_session_unpublish(session, g_publisher);
      otc_publisher_set_publish_video(g_publisher, false);      
      otc_session_disconnect(session);
      
    }    
    case ('s'):{
      if(screen_cap == 2){ //switch to screen cap if camera
        screen_cap = 1;
        otc_publisher_set_video_type(g_publisher, OTC_PUBLISHER_VIDEO_TYPE_SCREEN);
      }else{ //switch to cam if screen cap
        screen_cap = 2;
        otc_publisher_set_video_type(g_publisher, OTC_PUBLISHER_VIDEO_TYPE_CAMERA);
      }
    }        
  }
}


int main(int argc, char **argv)
{
    String apiKey;
    String sessionId;
    String token;
    RendererManager renderer_manager;
    screen_cap = 2; //camera first

    struct custom_capturer
    {
      const otc_video_capturer *video_capturer;
      struct otc_video_capturer_callbacks video_capturer_callbacks;
      int width;
      int height;
      otk_thread_t capturer_thread;
      std::atomic<bool> capturer_thread_exit;
    };

    if (argc == 1){
      std::cout << "Using 'config.h' for credentials" << std::endl;
      apiKey = API_KEY;
      sessionId = SESSION_ID;
      token = TOKEN;
    }
    else if (argc == 4) {
      std::cout << "Using parameters for credentials" << std::endl;
      apiKey = argv[1];
      sessionId = argv[2];
      token = argv[3];      
    }
    else if (argc < 4) {
      std::cerr << "Usage: " << argv[0] << "<apiKey> <sessionId> <token>" << std::endl;
      return EXIT_FAILURE;
    }

    std::cout << "API KEY: " << apiKey << std::endl;
    std::cout << "SESSION ID: " << sessionId << std::endl;
    std::cout << "TOKEN: " << token << std::endl;
  
  if (otc_init(nullptr) != OTC_SUCCESS)
  {
    std::cout << "Could not init OpenTok library" << std::endl;
    return EXIT_FAILURE;
  }

#ifdef CONSOLE_LOGGING
  otc_log_set_logger_callback(on_otc_log_message);
  otc_log_enable(OTC_LOG_LEVEL_ALL);
#endif
  

  struct otc_session_callbacks session_callbacks = default_session_callbacks;
  session_callbacks.user_data = &renderer_manager;
  session_callbacks.on_connected = on_session_connected;
  session_callbacks.on_stream_received = on_session_stream_received;

  session = otc_session_new(apiKey.c_str(), sessionId.c_str(), &session_callbacks);

  if (session == nullptr)
  {
    std::cout << "Could not create OpenTok session successfully" << std::endl;
    return EXIT_FAILURE;
  }

  struct custom_capturer *video_capturer = (struct custom_capturer *)malloc(sizeof(struct custom_capturer));
  video_capturer->video_capturer_callbacks = custom_capture_callbacks;
  video_capturer->video_capturer_callbacks.user_data = static_cast<void *>(video_capturer);
 

  video_capturer->width = 640;
  video_capturer->height = 480;

  struct otc_publisher_callbacks publisher_callbacks = default_publisher_callbacks;
  publisher_callbacks.user_data = &renderer_manager;
  publisher_callbacks.on_render_frame = on_publisher_render_frame;

  g_publisher = otc_publisher_new("opentok-linux-sdk-samples",
                                  &(video_capturer->video_capturer_callbacks),
                                  &publisher_callbacks);

  if (g_publisher == nullptr)
  {
    std::cout << "Could not create OpenTok publisher successfully" << std::endl;
    otc_session_delete(session);
    return EXIT_FAILURE;
  }
   renderer_manager.createNamedRenderer(g_publisher, (void*)"Press S to Switch between Cam and Screenshare. Press Q to quit." );
  //renderer_manager.createRenderer(g_publisher);


  otc_session_connect(session, token.c_str());

  renderer_manager.on_key_press = on_keypress_callback;

  renderer_manager.runEventLoop();
  

  if ((session != nullptr) && (g_publisher != nullptr) && g_is_publishing.load())
  {
    otc_session_unpublish(session, g_publisher);
  }

  if (g_publisher != nullptr)
  {
    otc_publisher_delete(g_publisher);
  }

  if ((session != nullptr) && g_is_connected.load())
  {
    otc_session_disconnect(session);
  }

  if (session != nullptr)
  {
    otc_session_delete(session);
  }

  if (video_capturer != nullptr)
  {
    free(video_capturer);
  }

  otc_destroy();
  
  return EXIT_SUCCESS;
}