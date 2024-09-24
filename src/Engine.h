#ifndef ENGINE_H
#define ENGINE_H

#include <vector>
#include <string>

#include "wx/event.h"

class wxTimer;

namespace Slic3r {
    namespace GUI{
        class GUI_App;
        class MainFrame;
        class Plater;

        class Snapmaker_Orca_Engine : public wxEvtHandler
        {
        public:
            // input the 3mf/.stl... files to engine
            Snapmaker_Orca_Engine(std::vector<std::string>& user_inputs);

            void init();

            void on_gui_loaded();

            void on_time_check();

            void run_engine();

            void do_next_task();

            void close_engine();

        private:
            void add_file_server(std::string& filePath);

            void slice_all_plates_server();

            void export_gcode_server(bool prefer_removable);

        private:
            std::vector<std::string> m_OriFiles;

            wxTimer* m_load_gui_timer = nullptr;

            wxTimer* m_check_export_timer = nullptr;

            wxTimer* m_delay_close_timer = nullptr;

        private:
            GUI_App* m_gui_app = nullptr;

            MainFrame* m_gui_main_frame = nullptr;

            Plater* m_gui_plater = nullptr;

            int m_task_index = 0;
        private:
            static int s_time_gui_load;
            static int s_time_check_export;
            static int s_time_delay_close;
        };
    }
}




#endif