#ifndef _slic3r_gui_FFUtils_hpp_
#define _slic3r_gui_FFUtils_hpp_

#include <map>
#include <string>
#include <wx/dc.h>
#include <wx/gdicmn.h>
#include <wx/string.h>

class wxWebView;

namespace Slic3r::GUI
{

enum FFPrinterPid { 
	ADVENTURER_5M     = 0x0023,
	ADVENTURER_5M_PRO = 0x0024,
	GUIDER_4          = 0x0025,
	AD5X              = 0x0026,
	GUIDER_4_PRO      = 0x0027,
    C5                = 0x0028,
    C5P               = 0x0029,
	ADVENTURER_A5     = 0x00BB,
	GUIDER_3_ULTRA    = 0x001F,
	OTHER             = 0xFFFF
};

struct FFPrinterPreset
{
    std::string bmp_file_name;
    std::string name;
    std::string model_id;
    FFPrinterPreset() {};
    FFPrinterPreset(std::string bmp_file_name, std::string name, std::string model_id)
		: bmp_file_name(bmp_file_name), name(name), model_id(model_id) {}
};

// How the device page lays out the temperature controls. Every model the device page knows maps
// to one of these; a model it does not know gets Generic (or Nozzles4 when it reports four
// nozzles), shown read-only so no temperature limit is guessed for it.
enum class FFTempLayout {
    Generic,         // top = nozzle, bottom = bed, mid = chamber (Adventurer 5M family, AD5X, Guider 4)
    Guider3Ultra,    // top = right nozzle, bottom = left nozzle, mid = bed
    Nozzles4,        // t1..t4 = nozzles, mid = bed (Creator 5)
    Nozzles4Chamber, // t1..t4 = nozzles, bottom = bed, mid = chamber (Creator 5 Pro)
};

struct com_dev_data_t;

struct FFPrinterSimpleData
{
    wxString name;
    int         comId;
    int         pid;
    bool        wan;
};

class FFUtils
{
public:
    static std::unordered_map<unsigned short, FFPrinterPreset> printer_preset_map;

	static wxString getBitmapFileName(unsigned short pid);

	static std::string getPrinterName(unsigned short pid);

	static std::string getPrinterModelId(unsigned short pid);

	// The product id of a connection; 0 when it is not known yet, OTHER for an unknown connection.
	static unsigned short getPid(int curId);
	static unsigned short getPid(const com_dev_data_t &data);
	// The rule behind getPid. The printer's own detail (devDetail->pid) wins when it has one: it
	// is what the printer reports about itself. Before the first detail, the id the connection
	// was started with (lanDevInfo.pid: a LAN scan, or the saved printer) stands in. A printer
	// added by address / serial / check code, or taken from the Printers list, starts with 0
	// there, so reading lanDevInfo.pid alone left the device page on "unknown model" for a
	// Creator 5 that had already said what it is (EDGESLICER-6).
	static unsigned short resolvePid(bool lan, unsigned short lanPid, bool hasDetail, int detailPid);
	static bool isKnownPid(unsigned short pid);
	// The temperature layout for a model; nozzleCnt (from the printer's detail, 0 when unknown)
	// only matters for a model this build does not know.
	static FFTempLayout tempLayout(unsigned short pid, int nozzleCnt);

	static bool isPrinterSupportAms(unsigned short pid);
    static bool isPrinterSupportCoolingFan(unsigned short pid);
    static bool isPrinterSupportDeviceFilter(unsigned short pid);
    static bool isNozzlesPrinter(unsigned short pid);
   
	static wxString convertStatus(const std::string& status);
	static wxString convertStatus(const std::string& status, wxColour& color);

	static wxString converDeviceError(const std::string &error);

	static std::string utf8Substr(const std::string& str, int start, int length);

	static std::string truncateString(const std::string &s, size_t length);
    static std::string wxString2StdString(const wxString& str);

	static wxString trimString(wxDC &dc, const wxString &str, int width);
    static wxString elideString(wxWindow* wnd, const wxString& str, int width);
	static wxString elideString(wxWindow* wnd, const wxString& str, int width, int lines);
	static wxString wrapString(wxWindow* wnd, const wxString& str, int width);
	static wxString wrapString(wxDC &dc, const wxString &str, int width);
	static int getStringLines(const wxString& str);

	static std::string flashforgeWebsite();
	static wxString privacyPolicy();
	static wxString userAgreement();
	static wxString userRegister();
	static wxString passwordForget();

	static wxRect calcContainedRect(const wxSize &containerSize, const wxSize &imgSize, bool enlarge);

	static std::string getTimestampMsStr();

	static std::string urlUnescape(const std::string &str);
	
	static long getHttpHeaders(const std::string &url, const std::vector<std::string> &keys,
		const std::string &userAgent, std::map<std::string, std::string> &headerMap, int msTimeout);

	static wxWebView *CreateWebView(wxWindow *parent);

	static std::unordered_map<std::string, FFPrinterSimpleData> getDevListForModelId(std::string modelId, bool include_printing = false);

    static std::unordered_map<std::string, FFPrinterSimpleData> getSelectPresetDevList(bool include_printing = false);

    static bool                                                 isLikeFilament(const wxString& str);

    static bool                                                 matchMaterialName(const wxString& str, const wxString& originStr);
};

}

#endif /* _slic3r_gui_FFUtils_hpp_ */
