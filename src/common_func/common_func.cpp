#include "common_func.hpp"
#include <boost/asio/ip/host_name.hpp>

#ifdef _WIN32
#include <windows.h>
#include <Shlobj.h>

#elif __APPLE__
#include <stdlib.h>
#endif

#include <fstream>
#include <nlohmann/json.hpp>

namespace common
{
    std::string get_pc_name()
    { 
        return boost::asio::ip::host_name();
    }

    std::string get_flutter_version()
    {

            std::string versionFilePath = "";

#ifdef _WIN32
            wchar_t appDataPath[MAX_PATH] = {0};
            auto    hr                    = SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appDataPath);
            char*   path                  = new char[MAX_PATH];
            size_t  pathLength;
            wcstombs_s(&pathLength, path, MAX_PATH, appDataPath, MAX_PATH);
            std::string filePath = path;
            versionFilePath      = filePath + "\\" + std::string("Snapmaker_Orca\\web\\flutter_web\\version.json");
#elif __APPLE__
            const char* home_env = getenv("HOME");
            versionFilePath      = home_env;
            versionFilePath      = versionFilePath + "/Library/Application Support/Snapmaker_Orca/web/flutter_web/version.json";
#else

#endif

            std::ifstream json_file(versionFilePath);
            if (!json_file.is_open()) {
                std::ifstream json_file(versionFilePath);                
                return "";
            }
            nlohmann::json json_data;
            json_file >> json_data;
            std::string str_version      = json_data["version"];
            std::string str_build_number = json_data["build_number"];

            std::string flutter_version = std::string("flutter_version: ") + str_version + std::string("  ") + std::string("build_number: ") +
                              str_build_number;
           
            return flutter_version;
    }

}