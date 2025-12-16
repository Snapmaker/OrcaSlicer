#ifndef _BURY_POINT_hpp_
#define _BURY_POINT_hpp_
#include <string>
#include <iostream>

#define BP_START_SOFT "bury_point_start_soft"
#define BP_SOFT_START_TIME "soft_start_time"
#define BP_SOFT_END_TIME "soft_end_time"

#define BP_DEIVCE_CONNECT "bury_point_device_connect"
#define BP_CONNECT_DEVICE_ID "device_id"
#define BP_CONNECT_NET_TYPE "net_type"

#define BP_LOGIN "bury_point_login"
#define BP_LOGIN_USER_ID "user_id"
#define BP_LOGIN_HTTP_CODE "http_code"

#define BP_VIDEO_START "bury_point_video_start"
#define BP_VIDEO_STATUS "video_status"

#define BP_DEVICE_ERROR "bury_point_device_error"
#define BP_DEVICE_ERROR_STATUS "error_status"

#define BP_UPLOAD "bury_point_upload"

#define BP_UPLOAD_AND_PRINT "bury_point_upload_and_print"

#define BP_COLOR_PAINTING "bury_point_color_painting"

#define BP_VIDEO_ABNORMAL "bury_point_video_abnormal"
//webview bury point



	static bool isAgreeSlice = false;
    extern std::string get_timestamp_seconds();


#endif