#include "FFUtils.hpp"
#include <cstdint>
#include <cctype>
#include <chrono>
#include <memory>
#include <utility>
#include <boost/algorithm/string/predicate.hpp>
#include <wx/webview.h>
#include <curl/curl.h>
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/FlashForge/MultiComMgr.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r::GUI
{

std::unordered_map<unsigned short, FFPrinterPreset> FFUtils::printer_preset_map = {
    {ADVENTURER_5M,     FFPrinterPreset("adventurer_5m",     "Adventurer 5M",            "Flashforge-Adventurer-5M")},
    {ADVENTURER_5M_PRO, FFPrinterPreset("adventurer_5m_pro", "Adventurer 5M Pro",        "Flashforge-Adventurer-5M-Pro")},
    {GUIDER_4,          FFPrinterPreset("guider4",           "Flashforge Guider 4",      "Flashforge-Guider4")},
    {AD5X,              FFPrinterPreset("ad5x",              "Flashforge AD5X",          "Flashforge-AD5X")}, 
    {GUIDER_4_PRO,      FFPrinterPreset("guider4_pro",       "Flashforge Guider4 Pro",   "Flashforge-Guider4-Pro")}, 
    {C5,                FFPrinterPreset("creator_5",         "Flashforge Creator 5",     "Flashforge-Creator-5")},
    {C5P,               FFPrinterPreset("creator_5_pro",     "Flashforge Creator 5 Pro", "Flashforge-Creator-5-Pro")},
    {ADVENTURER_A5,     FFPrinterPreset("adventurer_a5",     "Adventurer A5",            "Flashforge-Adventurer-A5")}, 
    {GUIDER_3_ULTRA,    FFPrinterPreset("guider_3_ultra",    "Guider 3 Ultra",           "Flashforge-Guider-3-Ultra")},
};

wxString FFUtils::getBitmapFileName(unsigned short pid)
{
    if (printer_preset_map.find(pid) != printer_preset_map.end()) {
        return printer_preset_map[pid].bmp_file_name;
    }
    return "";
}

std::string FFUtils::getPrinterName(unsigned short pid)
{
    if (printer_preset_map.find(pid) != printer_preset_map.end()) {
        return printer_preset_map[pid].name;
    }
    return "";
}

std::string FFUtils::getPrinterModelId(unsigned short pid)
{
    if (printer_preset_map.find(pid) != printer_preset_map.end()) {
        return printer_preset_map[pid].model_id;
    }
    return "";
}

unsigned short FFUtils::getPid(int curId) 
{ 
    bool                  valid = false;
    const com_dev_data_t& data  = MultiComMgr::inst()->devData(curId, &valid);
    if (!valid) {
        return OTHER;
    }
    return getPid(data);
}

unsigned short FFUtils::getPid(const com_dev_data_t &data)
{
    return resolvePid(data.connectMode == COM_CONNECT_LAN, data.lanDevInfo.pid, data.devDetail != nullptr,
                      data.devDetail != nullptr ? data.devDetail->pid : 0);
}

unsigned short FFUtils::resolvePid(bool lan, unsigned short lanPid, bool hasDetail, int detailPid)
{
    if (hasDetail && detailPid > 0 && detailPid < OTHER)
        return (unsigned short) detailPid;
    if (lan && lanPid != 0 && lanPid != OTHER)
        return lanPid;
    return 0;
}

bool FFUtils::isKnownPid(unsigned short pid)
{
    return printer_preset_map.find(pid) != printer_preset_map.end();
}

FFTempLayout FFUtils::tempLayout(unsigned short pid, int nozzleCnt)
{
    switch (pid) {
    case C5: return FFTempLayout::Nozzles4;
    case C5P: return FFTempLayout::Nozzles4Chamber;
    case GUIDER_3_ULTRA: return FFTempLayout::Guider3Ultra;
    default: break;
    }
    // A model this build does not know: four nozzles is the Creator 5 shape, anything else gets
    // the single-nozzle rows. Either way every row the layout names exists, so the page can fill it.
    if (!isKnownPid(pid) && nozzleCnt >= 4)
        return FFTempLayout::Nozzles4;
    return FFTempLayout::Generic;
}

bool FFUtils::isPrinterSupportAms(unsigned short pid)
{
    if (pid == AD5X || pid == GUIDER_4 || pid == GUIDER_4_PRO) {
        return true;
    }
    if (pid == C5 || pid == C5P) {
        return true;
    }
    return false;
}

bool FFUtils::isPrinterSupportCoolingFan(unsigned short pid)
{
    if (pid != AD5X) {
        return true;
    }
    return false;
}

bool FFUtils::isPrinterSupportDeviceFilter(unsigned short pid)
{
    if (pid == GUIDER_4 || pid == AD5X || pid == ADVENTURER_5M) {
        return false;
    }
    if (pid == C5 || pid == C5P) {
        return false;
    }
    return true;
}

bool FFUtils::isNozzlesPrinter(unsigned short pid) 
{ 
    switch (pid) {
    case C5:
    case C5P:
        return true;
    }
    return false; 
}

wxString FFUtils::convertStatus(const std::string& status)
{
    wxString st = _L("Idle");
    if ("offline" == status) {
        st = _L("Offline");
    } else {
        if ("printing" == status || "canceling" == status) {
            st = _L("Printing");
        } else if ("pause" == status || "pausing" == status) {
            st = _L("Paused");
        } else if ("error" == status) {
            st = _L("Error");
        } else if ("busy" == status || "calibrate_doing" == status || "heating" == status || "loading" == status) {
            st = _L("Busy");
        } else if ("cloud_slicing" == status || "downloading" == status || "unzipping" == status || "sending" == status) {
            st = _L("Busy");
        } else if ("completed" == status || "cancel" == status) {
            st = _L("Completed");
        }
        //} else if ("cancel" == status || "canceling" == status) {
        //    st = _L("Cancel");
        //    color = wxColour("#328DFB");
        //}
        //} else if ("heating" == rawstatus) {
        //    status = _L("Heating");
        //    //color = wxColour("");
        //}
    }
    return st;
}

wxString FFUtils::convertStatus(const std::string& status, wxColour& color)
{
	wxString st = _L("Idle");
    color = wxColour("#00CD6D");
    if ("offline" == status) {
        st = _L("Offline");
        color = wxColour("#999999");
    } else {
        if ("printing" == status || "canceling" == status) {
            st = _L("Printing");
            color = wxColour("#4D54FF");
        } else if ("pause" == status || "pausing" == status) {
            st = _L("Paused");
            color = wxColour("#982187");
        } else if ("error" == status) {
            st = _L("Error");
            color = wxColour("#FD4A29");
        } else if ("busy" == status || "calibrate_doing" == status || "heating" == status || "loading" == status) {
            st = _L("Busy");
            color = wxColour("#F9B61C");
        } else if ("cloud_slicing" == status || "downloading" == status || "unzipping" == status || "sending" == status) {
            st = _L("Busy");
            color = wxColour("#F9B61C");
        } else if ("completed" == status || "cancel" == status) {
            st = _L("Completed");
            color = wxColour("#328DFB");
        }
        //} else if ("cancel" == status || "canceling" == status) {
        //    st = _L("Cancel");
        //    color = wxColour("#328DFB");
        //}
        //} else if ("heating" == rawstatus) {
        //    status = _L("Heating");
        //    //color = wxColour("");
        //}
    }
    return st;
}

wxString FFUtils::converDeviceError(const std::string &error) 
{
    wxString st;
    if ("E0001" == error) {
        st = _L("The printer move out of range.Please go home again!");
    } else if ("E0002" == error) {
        st = _L("Lost communication with MCU eboard");
    } else if ("E0003" == error) {
        st = _L("TMC reports error:GSTAT");
    } else if ("E0004" == error) {
        st = _L("Unable to read tmc");
    } else if ("E0005" == error) {
        st = _L("MCU eboard: Unable to connect");
    } else if ("E0006" == error) {
        st = _L("Nozzle temperature error.");
    } else if ("E0007" == error) {
        st = _L("Extruder not heating at expected.Please check extruder.");
    } else if ("E0008" == error) {
        st = _L("Extruder not heating at expected.Please check extruder.");
    } else if ("E0009" == error) {
        st = _L("Platform not heating at expected.Please check platform.");
    } else if ("E0010" == error) {
        st = _L("Chamber not heating at expected.Please check chamber.");
    } else if ("E0011" == error) {
        st = _L("Host error, please restart !");
    } else if ("E0012" == error) {
        st = _L("Z-Axis go home error.");
    } else if ("E0013" == error) {
        st = _L("Y-Axis go home error.");
    } else if ("E0014" == error) {
        st = _L("X-Axis go home error.");
    } else if ("E0015" == error) {
        st = _L("Extruder temperature error !");
    } else if ("E0016" == error) {
        st = _L("Platform temperature error !");
    } else if ("E0017" == error) {
        st = _L("Move queue overflow");
    } else if ("E0018" == error) {
        st = _L("No filament");
    } else if ("E0115" == error) {
        st = _L("Lidar focus failure detected. Please check the Lidar.");
    }
    return st;
}

std::string FFUtils::utf8Substr(const std::string &str, int start, int length) 
{
    int bytes = 0;
    int i = 0;
    for (i = start, bytes = 0; i < str.length() && bytes < length; ++i) {
        if ((str[i] & 0xC0) != 0x80) {
            ++bytes;
        }
    }
    return str.substr(start, i - start);
}

std::string FFUtils::truncateString(const std::string &s, size_t length) 
{
    if (s.length() > length) {
        std::string transName = wxString::FromUTF8(s).ToStdString();
        std::string trunkName = utf8Substr(transName, 0, length);
        return trunkName + "...";
    } else {
        return wxString::FromUTF8(s).ToStdString();
    }
}

std::string FFUtils::wxString2StdString(const wxString& str)
{
    return std::string(str.utf8_str().data(), str.utf8_str().length()); 
}

wxString FFUtils::trimString(wxDC &dc, const wxString &str, int width)
{
    wxString clipText = str;
    int      clipw    = 0;
    if (dc.GetTextExtent(str).x > width) {
        for (int i = 0; i < str.length(); ++i) {
            clipText = str.Left(i) + wxT("...");
            clipw    = dc.GetTextExtent(clipText).x;
            if (clipw + dc.GetTextExtent(wxT("...")).x > width) {
                break;
            }
        }
    }
    return clipText;
}

wxString FFUtils::elideString(wxWindow* wnd, const wxString& str, int width)
{
    if (!wnd) return str;
    wxString elide_str = str;
    int      elide_width    = 0;
    if (wnd->GetTextExtent(str).x > width) {
        for (int i = 0; i < str.length(); ++i) {
            elide_str = str.Left(i) + wxT("...");
            elide_width    = wnd->GetTextExtent(elide_str).x;
            if (elide_width + wnd->GetTextExtent(wxT("...")).x > width) {
                break;
            }
        }
    }
    return elide_str;
}

wxString FFUtils::elideString(wxWindow* wnd, const wxString& str, int width, int lines)
{
    if (!wnd || wnd->GetTextExtent(str).x <= width) return str;
    if (lines <= 0) {
        lines = 1;
    }
    wxString elide_str;
    wxString _str = str;
    while (lines > 0 && !_str.empty()) {
        if (wnd->GetTextExtent(_str).x <= width) {
            elide_str += _str;
            break;
        }
        wxString append_str;
        int elide_width = 0;
        if (lines == 1) {
            append_str = "...";
            elide_width = wnd->GetTextExtent(append_str).x;
        }
        wxString tmp_str;
        for (size_t i = 0; i < _str.length(); ++i) {
            elide_width += wnd->GetTextExtent(_str[i]).x;
            if (elide_width > width) {
                if (lines == 1) {
                    elide_str += _str.Left(i) + append_str;
                    _str = "";
                } else {
                    elide_str += _str.Left(i) + "\n";
                    _str = _str.substr(i);
                }
                break;
            }
        }
        --lines;
    }
    return elide_str;
}

wxString FFUtils::wrapString(wxWindow* wnd, const wxString& str, int width)
{
    if (!wnd || wnd->GetTextExtent(str).x <= width) return str;

    wxString wrap_str;
    wxString _str = str;
    while (!_str.empty()) {
        if (wnd->GetTextExtent(_str).x <= width) {
            wrap_str += _str;
            break;
        }
        int wrap_width = 0;
        wxString tmp_str;
        for (size_t i = 0; i < _str.length(); ++i) {
            wrap_width += wnd->GetTextExtent(_str[i]).x;
            if (wrap_width > width) {
                wrap_str += _str.Left(i) + "\n";
                _str = _str.substr(i);
                break;
            }
        }
    }
    return wrap_str;
}

wxString FFUtils::wrapString(wxDC &dc, const wxString &str, int width)
{
    auto findFirstWrapPos = [](const wxString &str, size_t start) {
        for (size_t i = start + 1; i < str.size(); ++i) {
            if (isspace(str[i]) || str[i] == '-') {
                return i;
            }
        }
        return str.size();
    };
    wxString ret = str;
    for (size_t i = 0, j = findFirstWrapPos(ret, i); j != ret.size();) {
        size_t k = findFirstWrapPos(ret, j);
        if (dc.GetTextExtent(ret.Mid(i, k - i)).x > width) {
            ret.insert(j, 1, '\n');
            i = j + 1;
            j = k + 1;
        } else {
            j = k;
        }
    }
    return ret;
}

int FFUtils::getStringLines(const wxString& str)
{
    int lines = 1;
    size_t pos   = 0;
    while ((pos = str.find('\n', pos)) != wxString::npos) {
        ++lines;
        ++pos;
    }
    return lines;
}

std::string FFUtils::flashforgeWebsite()
{
    std::string code = Slic3r::GUI::wxGetApp().app_config->get("language");
    if (code == "zh_CN") {
        return "https://www.sz3dp.com";
    }
    return "https://www.flashforge.com";
}

wxString FFUtils::privacyPolicy()
{
    std::string code = Slic3r::GUI::wxGetApp().app_config->get("language");
    if (code == "zh_CN") {
        return "https://auth.flashforge.com/privacyPolicy";
    }
    return "https://auth.flashforge.com/en/privacyPolicy";
}

wxString FFUtils::userAgreement()
{
    std::string code = Slic3r::GUI::wxGetApp().app_config->get("language");
    if (code == "zh_CN") {
        return "https://auth.flashforge.com/userAgreement";
    }
    return "https://auth.flashforge.com/en/userAgreement";
}

wxString FFUtils::userRegister()
{
    std::string code = Slic3r::GUI::wxGetApp().app_config->get("language");
    if (code == "zh_CN") {
        return "https://auth.flashforge.com/zh/signUp/?channel=Orca";
    }else if(code.compare("fr_FR") == 0){
		return "https://auth.flashforge.com/fr/signUp/?channel=Orca";
	}else if(code.compare("es_ES") == 0){
		return "https://auth.flashforge.com/es/signUp/?channel=Orca";
	}else if(code.compare("de_DE") == 0){
		return "https://auth.flashforge.com/de/signUp/?channel=Orca";
	}else if(code.compare("ja_JP") == 0){
		return "https://auth.flashforge.com/ja/signUp/?channel=Orca";
	}else if(code.compare("ko_KR") == 0){
		return "https://auth.flashforge.com/ko/signUp/?channel=Orca";
	}else if(code.compare("lt_LT") == 0){
		return "https://auth.flashforge.com/lt/signUp/?channel=Orca";
	}
    return "https://auth.flashforge.com/en/signUp/?channel=Orca";
}

wxString FFUtils::passwordForget()
{
    std::string code = Slic3r::GUI::wxGetApp().app_config->get("language");
    if (code == "zh_CN") {
        return "https://auth.flashforge.com/zh/resetPassword/?channel=Orca";
    }else if(code.compare("fr_FR") == 0){
		return "https://auth.flashforge.com/fr/resetPassword/?channel=Orca";
	}else if(code.compare("es_ES") == 0){
		return "https://auth.flashforge.com/es/resetPassword/?channel=Orca";
	}else if(code.compare("de_DE") == 0){
		return "https://auth.flashforge.com/de/resetPassword/?channel=Orca";
	}else if(code.compare("ja_JP") == 0){
		return "https://auth.flashforge.com/ja/resetPassword/?channel=Orca";
	}else if(code.compare("ko_KR") == 0){
		return "https://auth.flashforge.com/ko/resetPassword/?channel=Orca";
	}else if(code.compare("lt_LT") == 0){
		return "https://auth.flashforge.com/lt/resetPassword/?channel=Orca";
	}
    return "https://auth.flashforge.com/en/resetPassword/?channel=Orca";
}

wxRect FFUtils::calcContainedRect(const wxSize &containerSize, const wxSize &imgSize, bool enlarge)
{
    if (imgSize.x == 0 || imgSize.y == 0) {
        return wxRect(0, 0, containerSize.x, containerSize.y);
    }
    wxSize drawSize;
    if (enlarge || imgSize.x > containerSize.x || imgSize.y > containerSize.y) {
        if (containerSize.x * imgSize.y > imgSize.x * containerSize.y) {
            drawSize.x = imgSize.x * containerSize.y / imgSize.y;
            drawSize.y = containerSize.y;
        } else {
            drawSize.x = containerSize.x;
            drawSize.y = imgSize.y * containerSize.x / imgSize.x;
        }
    } else {
        drawSize = imgSize;
    }
    wxRect rt;
    rt.x = containerSize.x / 2 - drawSize.x / 2;
    rt.y = containerSize.y / 2 - drawSize.y / 2;
    rt.width = drawSize.x;
    rt.height = drawSize.y;
    return rt;
}

std::string FFUtils::getTimestampMsStr()
{
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    int64_t msTime = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(msTime);
}

std::string FFUtils::urlUnescape(const std::string &str)
{
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        return str;
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> freeCurl(curl, curl_easy_cleanup);
    int outLength = 0;
    char *decodedStr = curl_easy_unescape(curl, str.c_str(), str.length(), &outLength);
    if (decodedStr == nullptr) {
        return str;
    }
    std::unique_ptr<char, decltype(&curl_free)> freeEscapeObjectName(decodedStr, curl_free);
    return std::string(decodedStr, outLength);
}

long FFUtils::getHttpHeaders(const std::string &url, const std::vector<std::string> &keys,
    const std::string &userAgent, std::map<std::string, std::string> &headerMap, int msTimeout)
{
    using client_data_t = std::pair<std::map<std::string, std::string> &, const std::vector<std::string> &>;
    size_t (*headerCallback)(char *, size_t, size_t, void *) =
        [](char *buffer, size_t size, size_t nitems, void *userData)-> size_t {
            auto &clientData = *(client_data_t *)userData;
            std::string header(buffer, size * nitems);
            for (auto &key : clientData.second) {
                bool equal = true;
                for (size_t i = 0; i < header.size() && i < key.size(); ++i) {
                    if (tolower(header[i]) != tolower(key[i])) {
                        equal = false;
                        break;
                    }
                }
                if (equal) {
                    clientData.first.emplace(key, header);
                    break;
                }
            }
            return size * nitems;
        };
    size_t (*writeCallback)(void *, size_t, size_t, void *) = 
        [](void *ptr, size_t size, size_t nmemb, void *userdata) {
            return (size_t)0;
        };
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        return -1;
    }
    curl_slist *curlSlist = curl_slist_append(nullptr, userAgent.c_str());
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> freeCurl(curl, curl_easy_cleanup);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> freeCurlSlist(curlSlist, curl_slist_free_all);
    client_data_t clientData(headerMap, keys);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    // Download links from the FlashForge web pages: the same certificate policy as every Http
    // request (checked for internet hosts, not for LAN ones). Was VERIFYPEER/VERIFYHOST 0.
    Slic3r::Http::apply_tls_policy(curl, url);
    //curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curlSlist);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &clientData);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)msTimeout);
    curl_easy_perform(curl);
    long responseCode = -1;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    return responseCode;
}

wxWebView *FFUtils::CreateWebView(wxWindow *parent)
{
    return WebView::CreateWebView(parent, wxEmptyString);
}

std::unordered_map<std::string, FFPrinterSimpleData> FFUtils::getDevListForModelId(std::string modelId, bool include_printing)
{
    std::unordered_map<std::string, FFPrinterSimpleData> list;
    bool                                       valid = false;
    com_id_list_t                              idList = MultiComMgr::inst()->getReadyDevList();
    auto can_sync_from_status = [include_printing](const std::string &status) {
        if (status.empty())
            return false;
        if (boost::algorithm::iequals(status, "ready"))
            return true;
        return include_printing &&
               (boost::algorithm::iequals(status, "printing") ||
                boost::algorithm::iequals(status, "running") ||
                boost::algorithm::iequals(status, "prepare") ||
                boost::algorithm::iequals(status, "slicing") ||
                boost::algorithm::iequals(status, "pausing") ||
                boost::algorithm::iequals(status, "pause") ||
                boost::algorithm::iequals(status, "canceling") ||
                boost::algorithm::iequals(status, "busy") ||
                boost::algorithm::iequals(status, "calibrate_doing") ||
                boost::algorithm::iequals(status, "heating") ||
                boost::algorithm::iequals(status, "loading") ||
                boost::algorithm::iequals(status, "cloud_slicing") ||
                boost::algorithm::iequals(status, "downloading") ||
                boost::algorithm::iequals(status, "unzipping") ||
                boost::algorithm::iequals(status, "sending"));
    };
    for (auto id : idList) {
        auto data = MultiComMgr::inst()->devData(id, &valid);
        if (valid && data.devDetail != nullptr) {
            std::string dev_id, status;
            FFPrinterSimpleData mdata;
            mdata.comId = id;
            mdata.wan  = data.connectMode;
            if (COM_CONNECT_LAN == data.connectMode) {
                dev_id     = data.lanDevInfo.serialNumber;
                mdata.pid  = getPid(data);
                mdata.name = wxString::FromUTF8(data.devDetail->name);
                status     = data.devDetail->status;
            } else if (COM_CONNECT_WAN == data.connectMode) {
                dev_id     = data.wanDevInfo.serialNumber;
                mdata.pid  = getPid(data);
                mdata.name = wxString::FromUTF8(data.devDetail->name);
                status     = data.wanDevInfo.status;
            }
            if (modelId != getPrinterModelId(mdata.pid)) {
                continue;
            }
            if (can_sync_from_status(status)) {
                auto iter = list.find(dev_id);
                if (iter == list.end()) {
                    list.emplace(dev_id, mdata);
                } else if (COM_CONNECT_LAN == data.connectMode) {
                    iter->second = mdata;
                }
            }
        } else {
            BOOST_LOG_TRIVIAL(warning) << "com_id (" << id << "): get com data error";
        }
    }
    return list;
}

std::unordered_map<std::string, FFPrinterSimpleData> FFUtils::getSelectPresetDevList(bool include_printing)
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr) {
        return {};
    }
    std::string model_id = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
    return getDevListForModelId(model_id, include_printing);
}

bool FFUtils::isLikeFilament(const wxString& str) {
    return str == "TPU";
}

namespace {

std::string compact_material_text(const wxString &str)
{
    std::string raw = std::string(str.utf8_string());
    std::string compact;
    compact.reserve(raw.size());
    for (unsigned char ch : raw) {
        if (std::isalnum(ch))
            compact.push_back(char(std::toupper(ch)));
    }
    return compact;
}

std::string canonical_material_name(const wxString &str)
{
    const std::string compact = compact_material_text(str);
    if (compact.empty())
        return {};

    static constexpr const char *known_materials[] = {
        "SUPPORT", "BVOH", "PETG", "PCTG", "HIPS", "PLA", "ABS", "ASA",
        "TPU", "PVA", "PET", "PPS", "PA", "PC", "PP", "PE"
    };

    for (const char *material : known_materials) {
        if (compact.find(material) != std::string::npos)
            return material;
    }
    return {};
}

} // namespace

bool FFUtils::matchMaterialName(const wxString& str, const wxString& originStr)
{
    const wxString material = str.Strip(wxString::both);
    const wxString origin   = originStr.Strip(wxString::both);
    if (material.empty() || origin.empty())
        return false;

    if (material.IsSameAs(origin, false))
        return true;

    const std::string material_name = canonical_material_name(material);
    const std::string origin_name   = canonical_material_name(origin);
    if (!material_name.empty() && !origin_name.empty())
        return material_name == origin_name;

    return compact_material_text(material) == compact_material_text(origin);
}

} // end namespace
