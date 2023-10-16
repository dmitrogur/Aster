#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include "wav.h"
#include <gui/widgets/folder_select.h>
#include <recorder_interface.h>
#include <dsp/routing/splitter.h>
#include <dsp/audio/volume.h>
#include <dsp/bench/peak_level_meter.h>
#include "../../decoder_modules/radio/src/radio_interface.h"
// decoder_modules/radio/src/
#include <regex>
#include <core.h>
#include <ctime>
#include <chrono>
#include <gui/dialogs/dialog_box.h>
#include <utils/freq_formatting.h>

#include <cmath> // для round
#include <iomanip>

#include <unistd.h>

#include <curl/curl.h>

struct FindedBookmark {
    double frequency;    
    float bandwidth;
    int mode;
    int level;
    bool selected;
    double left_freq;
    double right_freq; 
};

struct FindedFreq {
    double frequency;    
    float level;
    bool selected;
};

ConfigManager config;

struct SearchMode {
    std::string listName;
    int    _mode;
    double _bandwidth;
    double _startFreq;
    double _stopFreq;
    double _interval;
    double _passbandRatio;
    int    _tuningTime;//  = 350;
    bool   _status_stop; // = false;
    int    _waitingTime; //  = 1000
    bool   _status_record;
    int    _lingerTime; // 
    bool   _status_ignor; // 
    float  _level; //  = -70.0
    bool selected;    
};


SDRPP_MOD_INFO{
    /* Name:            */ "scanner2",
    /* Description:     */ "Frequency scanner2 for Aster",
    /* Author:          */ "DGurevuch",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1
};

std::string getNow(){
    time_t now = time(0);
    tm* ltm = localtime(&now);
    char buf[1024];
    sprintf(buf, "%02d/%02d/%02d %02d:%02d:%02d", ltm->tm_mday, ltm->tm_mon + 1, ltm->tm_year + 1900, ltm->tm_hour, ltm->tm_min, ltm->tm_sec);
    std::string prefix ="";
    return prefix + buf;
}

std::string genLogFileName(std::string prefix) {
    time_t now = time(0);
    tm* ltm = localtime(&now);
    char buf[1024];
    sprintf(buf, "%02d-%02d-%02d_%02d-%02d-%02d.log", ltm->tm_year + 1900, ltm->tm_mon + 1, ltm->tm_mday, ltm->tm_hour, ltm->tm_min, ltm->tm_sec);
    return prefix + buf;
}


class ScannerModule2 : public ModuleManager::Instance {
public:
    ScannerModule2(std::string name) {
        this->name = name;
        gui::menu.registerEntry(name, menuHandler, this, NULL);
        root = (std::string)core::args["root"];
        
        finded_freq.clear(); 
        listmaxLevel.clear();

        config.acquire();
        std::string selList = config.conf["selectedList"];
        config.release();

        refreshLists();
        loadByName(selList);


        logicList.clear();
        logicListTxt = "";
        logicList.push_back("Налаштування");
        logicListTxt += "Налаштування";
        logicListTxt += '\0';


        logicList.push_back("Звичайний пошук");
        logicListTxt += "Звичайний пошук";
        logicListTxt += '\0';

        logicList.push_back("Адаптивний  пошук");
        logicListTxt += "Адаптивний  пошук";
        logicListTxt += '\0';

// 2,5; 5; 10; 12,5; 20; 25; 30 или 50 кГц.
        intervalsList.clear();
        intervalsListTxt = "";
        intervalsList.push_back(1000);
        intervalsListTxt += "1000";
        intervalsListTxt += '\0';

        intervalsList.push_back(2500);
        intervalsListTxt += "2500";
        intervalsListTxt += '\0';

        intervalsList.push_back(5000);
        intervalsListTxt += "5000";
        intervalsListTxt += '\0';

        intervalsList.push_back(6250);
        intervalsListTxt += "6250";
        intervalsListTxt += '\0';

        intervalsList.push_back(10000);
        intervalsListTxt += "10000";
        intervalsListTxt += '\0';

        intervalsList.push_back(12500);
        intervalsListTxt += "12500";
        intervalsListTxt += '\0';

        intervalsList.push_back(20000);
        intervalsListTxt += "20000";
        intervalsListTxt += '\0';

        intervalsList.push_back(25000);
        intervalsListTxt += "25000";
        intervalsListTxt += '\0';

        intervalsList.push_back(30000);
        intervalsListTxt += "30000";
        intervalsListTxt += '\0';

        intervalsList.push_back(50000);
        intervalsListTxt += "50000";
        intervalsListTxt += '\0';

        intervalsList.push_back(100000);
        intervalsListTxt += "100000";
        intervalsListTxt += '\0';

        selectedIntervalId = 0;
        for (int i = 0; i < intervalsList.size(); i++) {
            if (intervalsList[i] == interval) {
                selectedIntervalId = i;
                break;
            }
        }


        bandwidthsList.clear();
        bandwidthsList.push_back(1000);
        bandwidthsList.push_back(6250);
        bandwidthsList.push_back(12500);
        bandwidthsList.push_back(25000);
        bandwidthsList.push_back(50000);
        bandwidthsList.push_back(100000);
        bandwidthsList.push_back(220000);


        thisURL = core::configManager.conf["Url"];
        thisInstance = core::configManager.conf["InstanceName"];
        thisInstance = thisInstance + "-2";
    }

    ~ScannerModule2() {
        gui::menu.removeEntry(name);
        stop();
    }

    void postInit() {}
    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }


private:

    static void applyBookmark(FindedBookmark bm, std::string vfoName) {
        if (vfoName == "") {
            // TODO: Replace with proper tune call
            gui::waterfall.setCenterFrequency(bm.frequency);
            gui::waterfall.centerFreqMoved = true;
        }        
        else {            
            if (core::modComManager.interfaceExists(vfoName)) {
                if (core::modComManager.getModuleName(vfoName) == "radio") {
                    int mode = bm.mode;
                    float bandwidth = bm.bandwidth;
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                }
            }            
            tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, bm.frequency);
        }
    }

    static void menuHandler(void* ctx) {
        ScannerModule2* _this = (ScannerModule2*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        int _work;
        core::modComManager.callInterface("Запис", MAIN_GET_PROCESSING, NULL, &_work);
        bool _run = _this->running;
        if (_run || _work>0) { ImGui::BeginDisabled(); }

        // TODO: Replace with something that won't iterate every frame
        float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        float btnSize = ImGui::CalcTextSize("Додати набір").x + 4;

        if (_this->selectedLogicId>0) { //  ImGui::BeginDisabled(); }

            ImGui::LeftLabel("                                 ");
            ImGui::SameLine();
            // ImGui::TableSetColumnIndex(1);

            //========================
            if (ImGui::Button(("Додати набір##scann3_look" + _this->name).c_str(), ImVec2(btnSize, 0))) {
                SearchMode addList;
                addList.listName = _this->selectedListName;
                addList._startFreq = _this->startFreq;
                addList._stopFreq = _this->stopFreq;
                _this->working_list[_this->selectedListName] = addList;    
                _this->_count_list++;
            }
            // ImGui::EndTable();

            ImGui::SameLine();
            // ImGui::TableSetColumnIndex(2);
            if (ImGui::Button(("Очистити завдання##_list_clear_2" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) ) {
                _this->working_list.clear(); 
                _this->_count_list = 0;
                _this->_curr_list =0; 
            }
            // ImGui::EndTable();

            ImGui::LeftLabel("Завдання:");
        
            ImGui::SameLine();
            float _Size =  menuWidth  - (2 * lineHeight) - ImGui::CalcTextSize("Завдання:").x + 24; //  - 24 - btnSize; // menuWidth - (4 * lineHeight) - 6;
            if (ImGui::BeginTable(("scanner2_sbanks_table_2" + _this->name).c_str(), 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(_Size, 120))) {
                ImGui::TableSetupColumn("Назва");
                ImGui::TableSetupColumn("Старт, кГц");
                ImGui::TableSetupColumn("Стоп, кГц");

                ImGui::TableSetupScrollFreeze(2, 1);
                ImGui::TableHeadersRow();

                // _this->loadByName(_this->listNames[_this->selectedListId]);
                // config.conf["selectedList"] = _this->selectedListName;

                for (const auto& [bmName, bm] : _this->working_list) { // [ config.workconf["lists"].items()) {            
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    std::string listName = bm.listName;
                    ImGui::Text("%s", listName.c_str());

                    ImGui::TableSetColumnIndex(1);
                    int f = bm._startFreq;
                    unsigned int bw = ceil(f/1000);
                    ImGui::Text("%u", bw);

                    ImGui::TableSetColumnIndex(2);                
                    f = bm._stopFreq;                 
                    bw = ceil(f/1000);
                    ImGui::Text("%d",bw);

                    ImVec2 max = ImGui::GetCursorPos();
                }
          
                ImGui::EndTable();
            }
        

        }    
        //if (_this->selectedLogicId==0) { ImGui::EndDisabled(); }

        //=================================================================================================

        ImGui::LeftLabel("Режим пошуку:");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(("##logic_scanner2" + _this->name).c_str(), &_this->selectedLogicId, _this->logicListTxt.c_str())) {
            _this->selectedLogicId = _this->selectedLogicId;
            if (_this->selectedLogicId==0)
            {
               _this->status_direction  = false;
               _this->status_record = false;
            } else {
                _this->status_direction = true;
                _this->status_record = true;
            }
            flog::info("TRACE. _this->selectedLogicId = {0}!", _this->selectedLogicId);
        }


        std::vector<std::string> selectedNames;
        selectedNames.push_back("General");


        //float btnSize = ImGui::CalcTextSize("Зберегти як").x + ImGui::CalcTextSize("Зберегти").x + 20;
        btnSize = ImGui::CalcTextSize("Зберегти набір").x + 20;
        ImGui::SetNextItemWidth(menuWidth - 24 - (2 * lineHeight) - btnSize);
        if (ImGui::Combo(("##step_scanner2_list_sel" + _this->name).c_str(), &_this->selectedListId, _this->listNamesTxt.c_str())) {
            _this->loadByName(_this->listNames[_this->selectedListId]);
            config.acquire();
            config.conf["selectedList"] = _this->selectedListName;
            config.release(true);
        }

        if (_run || _work>0) { ImGui::EndDisabled(); }

        // 1 beginDisabled        
        if (_run || _work>0 || _this->listNames.size() == 0) { style::beginDisabled(); }
        /*
        ImGui::SameLine();
        btnSize = ImGui::CalcTextSize("Зберегти як").x + 4;
        if (ImGui::Button(("Зберегти як##scann3_save" + _this->name).c_str(), ImVec2(btnSize, 0))) {
            SearchMode addList;
            addList.listName = _this->selectedListName;
            addList._startFreq = _this->startFreq;
            addList._stopFreq = _this->stopFreq;
            _this->working_list[_this->selectedListName] = addList;    
            _this->_count_list++;
        }
        */
        ImGui::SameLine();
        //btnSize = ImGui::CalcTextSize("Зберегти налаштування").x + 4;
        if (ImGui::Button(("Зберегти набір##scann3_ren_lst_3" + _this->name).c_str(), ImVec2(btnSize, 20))) {
            _this->firstEditedListName = _this->listNames[_this->selectedListId];
            _this->editedListName = _this->firstEditedListName;
            _this->renameListOpen = true;            
        }
        if (_run || _work>0 || _this->listNames.size() == 0) { style::endDisabled(); }
        // 1 endDisabled        

        if (_run || _work>0) { ImGui::BeginDisabled(); }
        ImGui::SameLine();
        if (ImGui::Button(("+##scann3_add_lst_3" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
            // Find new unique default name
            
            if (std::find(_this->listNames.begin(), _this->listNames.end(), "New List") == _this->listNames.end()) {
                _this->editedListName = "New List";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    sprintf(buf, "New List (%d)", i);
                    if (std::find(_this->listNames.begin(), _this->listNames.end(), buf) == _this->listNames.end()) { break; }
                }
                _this->editedListName = buf;   
            }
            _this->newListOpen = true;            
        }
        if (_run || _work>0) { ImGui::EndDisabled(); }

        // 2 beginDisabled        
        if (_run || _work>0 || _this->selectedListName == "" || _this->selectedListName == "General") { style::beginDisabled(); }
        ImGui::SameLine();
        if (ImGui::Button(("-##scann3_del_lst_3" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
            _this->deleteListOpen = true;
        }
        if (_run || _work>0 || _this->selectedListName == "" || _this->selectedListName == "General") { style::endDisabled(); }
        // 2 endDisabled        
        
        if (_run || _work>0) { ImGui::BeginDisabled(); }
        // List delete confirmation
        if (ImGui::GenericDialog(("scann3_del_list_confirm3" + _this->name).c_str(), _this->deleteListOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::Text("Видалення банку \"%s\". Ви впевнені?", _this->selectedListName.c_str());
            }) == GENERIC_DIALOG_BUTTON_YES) {
            config.acquire();
            config.conf["lists"].erase(_this->selectedListName);
            config.release(true);
            _this->refreshLists();
            _this->selectedListId = std::clamp<int>(_this->selectedListId, 0, _this->listNames.size());
            if (_this->listNames.size() > 0) {
                _this->loadByName(_this->listNames[_this->selectedListId]);
                flog::info("TRACE. interval = {0}!", _this->interval);

            }
            else {
                _this->selectedListName = "";
            }
        }

        ImGui::LeftLabel("Режим     ");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(("##scan_edit_mode_3" + _this->name).c_str(), &_this->mode, demodModeListTxt)) {
            _this->mode = _this->mode;
            if(_this->mode==7) {
               // _raw = true;
               _this->bandwidth = 220000.0;
               _this->_bandwidthId = 6;
            } else {
               // _raw =false;
            }       
            if(_this->mode==0) _this->_bandwidthId = 2;
            if(_this->mode==1) _this->_bandwidthId = 6;
            if(_this->mode==2) _this->_bandwidthId = 0;
            if(_this->mode==3) _this->_bandwidthId = 1;
            if(_this->mode==4) _this->_bandwidthId = 1;
            if(_this->mode==5) _this->_bandwidthId = 1;
            if(_this->mode==6) _this->_bandwidthId = 1;

            flog::info("TRACE. mode = {0}, getSampleRate() {1} !", _this->mode, sigpath::iqFrontEnd.getSampleRate());
        }

        ImGui::LeftLabel("Смуга, Гц");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(("##scan_edit_bw_3" + _this->name).c_str(), &_this->_bandwidthId, bandListTxt)) {
                _this->bandwidth = _this->bandwidthsList[_this->_bandwidthId];
                flog::info("TRACE. ebandwidth = {0} !", _this->bandwidth);
            }


        ImGui::LeftLabel("Старт");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputDouble("##start_freq_scanner_2", &_this->startFreq, 100.0, 100000.0, "%0.0f")) {
            _this->startFreq = round(_this->startFreq);
        }
        ImGui::LeftLabel("Стоп  ");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputDouble("##stop_freq_scanner_2", &_this->stopFreq, 100.0, 100000.0, "%0.0f")) {
            _this->stopFreq = round(_this->stopFreq);
        }

        ImGui::LeftLabel("Крок");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(("##step_scanner2" + _this->name).c_str(), &_this->selectedIntervalId, _this->intervalsListTxt.c_str())) {
            _this->interval = _this->intervalsList[_this->selectedIntervalId];
            flog::info("TRACE. interval = {0}!", _this->interval);
        }
/*
        ImGui::LeftLabel("Крок2");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputDouble("##interval_scanner_2", &_this->interval, 100.0, 100000.0, "%0.0f")) {
            _this->interval = round(_this->interval);
        }
*/
        ImGui::LeftLabel("Коеф. смуги пропускання (%)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputDouble("##pb_ratio_scanner_2", &_this->passbandRatio, 1.0, 10.0, "%0.0f")) {
            _this->passbandRatio = std::clamp<double>(round(_this->passbandRatio), 1.0, 100.0);
        }
        ImGui::LeftLabel("Тривалість налаштування (мс)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt("##tuning_time_scanner_2", &_this->tuningTime, 100, 1000)) {
            _this->tuningTime = std::clamp<int>(_this->tuningTime, 100, 10000.0);
        }

        ImGui::Checkbox("Призупиняти пошук при виявленні сигналу##_status_scanner2", &_this->status_stop); // status_stop

        if (_run || _work>0) { ImGui::EndDisabled(); }

        //---------------------------------------------------
        if (!_this->status_stop || _run || _work>0) { ImGui::BeginDisabled(); }
        ImGui::LeftLabel("Макс. час очікування сигналу (мс)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt("##linger_timeWait_scanner_2", &_this->_waitingTime, 100, 1000)) {
            _this->_waitingTime = std::clamp<int>(_this->_waitingTime, 100, 100000.0);
        }
        if (!_this->status_stop || _run || _work>0) { ImGui::EndDisabled(); }
        //---------------------------------------------------

        if (_run || _work>0) { ImGui::BeginDisabled(); }
     
        ImGui::Checkbox("Пеленгувати##_status_direction_scanner2", &_this->status_direction); // status_stop
        ImGui::SameLine();

        if(ImGui::Checkbox("Реєструвати##_record_2", &_this->status_record)){
            _this->status_record = _this->status_record;
        }

        if (_this->status_record) {
            ImGui::LeftLabel("Тривалість запису (мс)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputInt("##linger_time_scanner_2", &_this->_lingerTime, 100, 1000)) {
                _this->_lingerTime = std::clamp<int>(_this->_lingerTime, 100, 10000.0);
            }            
        } else {
            ImGui::LeftLabel("Тривалість затримки (мс)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputInt("##linger_time_scanner_2_2", &_this->_lingerTime, 100, 1000)) {
                _this->_lingerTime = std::clamp<int>(_this->_lingerTime, 100, 10000.0);
            }
        }
        
        ImGui::Checkbox("Ігнорувати відомі частоти##_status_ignor_scanner2", &_this->status_ignor); // status_stop


        if (_this->selectedLogicId>0) { //  ImGui::BeginDisabled(); }

        //ImGui::BeginTable(("scanner2_tmp" + _this->name).c_str(), 3);
        //ImGui::TableNextRow();
        //ImGui::TableSetColumnIndex(0);
        ImGui::LeftLabel("                                 ");
        ImGui::SameLine();
        // ImGui::TableSetColumnIndex(1);

        //========================
        btnSize = ImGui::CalcTextSize("Стежити").x + 4;
        if (ImGui::Button(("Стежити##scann3_look" + _this->name).c_str(), ImVec2(btnSize, 0))) {
            SearchMode addList;
            addList.listName = _this->selectedListName;
            addList._startFreq = _this->startFreq;
            addList._stopFreq = _this->stopFreq;
            _this->working_list[_this->selectedListName] = addList;    
            _this->_count_list++;
        }
        // ImGui::EndTable();

        ImGui::SameLine();
        // ImGui::TableSetColumnIndex(2);
        if (ImGui::Button(("Очистити список ##_list_clear_2" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) ) {
            _this->working_list.clear(); 
            _this->_count_list = 0;
            _this->_curr_list =0; 
        }
        // ImGui::EndTable();

        ImGui::LeftLabel("Завдання:");
        
        ImGui::SameLine();
        float _Size =  menuWidth  - (2 * lineHeight) - ImGui::CalcTextSize("Завдання:").x + 24; //  - 24 - btnSize; // menuWidth - (4 * lineHeight) - 6;
        if (ImGui::BeginTable(("scanner2_sbanks_table_2" + _this->name).c_str(), 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(_Size, 120))) {
            ImGui::TableSetupColumn("Назва");
            ImGui::TableSetupColumn("Старт, кГц");
            ImGui::TableSetupColumn("Стоп, кГц");

            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableHeadersRow();

            // _this->loadByName(_this->listNames[_this->selectedListId]);
            // config.conf["selectedList"] = _this->selectedListName;

            for (const auto& [bmName, bm] : _this->working_list) { // [ config.workconf["lists"].items()) {            
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                std::string listName = bm.listName;
                ImGui::Text("%s", listName.c_str());

                ImGui::TableSetColumnIndex(1);
//                ImGui::Text("%s", demodModeList[fbm.mode]);
//                ImGui::Text("%s",std::to_string(bm["_startFreq"]).c_str());
                int f = bm._startFreq;
                unsigned int bw = ceil(f/1000);
                ImGui::Text("%u", bw);

                ImGui::TableSetColumnIndex(2);                
                f = bm._stopFreq;                 
                bw = ceil(f/1000);
                ImGui::Text("%d",bw);

                ImVec2 max = ImGui::GetCursorPos();
            }
          
            ImGui::EndTable();
        }
        

        }    
        //if (_this->selectedLogicId==0) { ImGui::EndDisabled(); }

        //=================================================================================================
        if (_this->running || _work>0) { ImGui::EndDisabled(); }
        
        ImGui::LeftLabel("Поріг виявлення");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        // ImGui::SliderFloat("##scanner_level", &_this->level, -150.0, 0.0);
        if(ImGui::SliderInt("##scanner2_level", &_this->intLevel, -150, 0)) {
            _this->level = _this->intLevel;
        }

        int _air_recording;
        core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);

        if (!_this->running) {
                if(_work>0)  ImGui::BeginDisabled(); 
                    if(_air_recording==1)  {
                        if (ImGui::Button("СТАРТ##scanner2_start_2", ImVec2(menuWidth, 0))) {
                            if(_this->working_list.empty()) {
                                SearchMode addList;
                                addList.listName = _this->selectedListName;
                                addList._startFreq = _this->startFreq;
                                addList._stopFreq = _this->stopFreq;
                                _this->working_list[_this->selectedListName] = addList;    
                                _this->_count_list++;
                            }
                           _this->start();
                        }
                    } else {
                        style::beginDisabled();
                        ImGui::Button("СТАРТ##scanner2_start_2", ImVec2(menuWidth, 0));
                        style::endDisabled();
                    }    
                if(_work>0)  ImGui::EndDisabled(); 

            ImGui::BeginTable(("scanner_status_add_bank" + _this->name).c_str(), 2);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Статус: Неактивний");
            ImGui::TableSetColumnIndex(1);
            ImGui::EndTable();

        }
        else {
            if(_air_recording==0)  {
                _this->stop();
            }

            if (ImGui::Button("СТОП ##scanner_start_2", ImVec2(menuWidth, 0))) {
                _this->stop();
            }
            ImGui::BeginTable(("scanner_status_add_bank" + _this->name).c_str(), 2);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (_this->_recording) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Статус: Реєстрація");
            } else if (_this->_Receiving) { //_finding
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Статус: Приймання");
            }
            else if (_this->tuning) {
                ImGui::TextColored(ImVec4(0, 1, 1, 1), "Статус: Тюнінг");
            }
            else {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "Статус: Сканування");
            }
            bool _rec = _this->_Receiving; // _Receiving;
            if (_rec==true && _this->selectedLogicId>0) { //  ImGui::BeginDisabled(); }
            ImGui::TableSetColumnIndex(1);

            if (ImGui::Button(("Додати у банк ##_freq_add_2" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) ) {
                int tmode =7;
                struct FreqData {
                    int freq;
                    int mode;
                    float level;
                } pFreqData;
                pFreqData.freq = (floor((_this->current+500)/1000.0)*1000);
                pFreqData.level = _this->level;
               flog::info("add freq {0} / {1} / {2}", _this->current, floor(_this->current/1000.0),  pFreqData.freq); 
//                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio") {
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &tmode);                    
                }
                pFreqData.mode = tmode;

                core::modComManager.callInterface("Сканування", pFreqData.freq , (void *) &pFreqData, NULL);

                // _this->importOpen = true;
                // _this->importDialog = new pfd::open_file("Import bookmarks", "", { "JSON Files (*.json)", "*.json", "All Files", "*" }, true);
            }         
            // if (_rec==false || _this->selectedLogicId==0) { ImGui::EndDisabled(); }
            }

            ImGui::EndTable();
        }

        ImGui::BeginTable(("scanner2_result" + _this->name).c_str(), 2);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ImVec4(1, 0, 1, 1), "         Результати пошуку:     ");
        ImGui::TableSetColumnIndex(1);

        if (ImGui::Button(("Очистити результати##_freq_clear_2" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) ) {
            _this->finded_freq.clear(); 
        }

        ImGui::EndTable();

            bool _rec = _this->running;
            if (_rec==true) { ImGui::BeginDisabled(); }

        if (ImGui::BeginTable(("scanner2_bkm_table_2" + _this->name).c_str(), 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 150))) {
            ImGui::TableSetupColumn("Частота, МГц");
            ImGui::TableSetupColumn("Режим");
            ImGui::TableSetupColumn("Смуга, кГц");
            ImGui::TableSetupColumn("Поріг, дБ");

            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableHeadersRow();

            for (const auto& [key, fbm] : _this->finded_freq) {            
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ///  ImGui::Text("%s", utils::formatFreq(key).c_str());

                ImVec2 min = ImGui::GetCursorPos();
                if (ImGui::Selectable((utils::formatFreqMHz(key) + "##scanner2_bkm_name" + _this->name).c_str(), &fbm.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick)) {
                    // if shift or control isn't pressed, deselect all others
                    if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) {
                        for (auto& [_name, _bm] : _this->finded_freq) {
                            if (key == _name) { continue; }
                            _bm.selected = false;
                        }
                    }
                }
                if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    applyBookmark(fbm, gui::waterfall.selectedVFO);
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", demodModeList[fbm.mode]);

//               ImGui::TableSetColumnIndex(1);
//               ImGui::Text("%s %s", utils::formatFreqMHz(bm.frequency).c_str(), demodModeList[bm.mode]);

                ImGui::TableSetColumnIndex(2);
                int bw = round(fbm.bandwidth/1000);
                std::string sbw = std::to_string(bw);
                if(bw==13) sbw ="12.5";
                if(bw==6) sbw ="6.25";

                ImGui::Text("%s", sbw.c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s",std::to_string(fbm.level).c_str());

                ImVec2 max = ImGui::GetCursorPos();
            }
          
            ImGui::EndTable();
        }
        if (_rec==true) { ImGui::EndDisabled(); }



        if (_this->selectedListName == "") { style::endDisabled(); }
/*
        if (_this->createOpen) {
            _this->createOpen = _this->bookmarkEditDialog();
        }

        if (_this->editOpen) {
            _this->editOpen = _this->bookmarkEditDialog();
        }
*/
        if (_this->newListOpen) {
            _this->newListOpen = _this->newListDialog();
        }

        if (_this->renameListOpen) {
            _this->renameListOpen = _this->newListDialog();
        }
/*
        if (_this->selectListsOpen) {
            _this->selectListsOpen = _this->selectListsDialog();
        }
*/          
    }

    void start() {
        if (running) { return; }

        int _air_recording;
        core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);
        flog::info("AIR Recording is '{0}'", _air_recording);
        if(_air_recording==0)  {
            return;
        }

        if(selectedLogicId>0) {
            for (const auto& [bmName, bm] : working_list) {
                loadByName(bmName);
                config.acquire();
                config.conf["selectedList"] = bmName;
                config.release(true);
                _curr_list = 0;
                // current = bm._startFreq;;
                break;
            }
        }  else {
            _count_list =0;
        }              
        current = startFreq;
        running = true;
        _recording = false;
        std::string folderPath = "%ROOT%/recordings";
        expandedLogPath = expandString(folderPath + genLogFileName("/scan_"));
        curl = NULL;
        
        try {
            curl = curl_easy_init();  
        }catch (...) {

        }
        if (core::modComManager.interfaceExists(gui::waterfall.selectedVFO)) {
            flog::info("core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &{0}, NULL); bandwidth = {1}", mode, bandwidth);
            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
            float band;
            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_BANDWIDTH, NULL, &band);
            flog::info("bandwidth = {0}/{1}", band, bandwidth);
        }  
        core::modComManager.callInterface("Запис", MAIN_SET_START, NULL, NULL);
        
        workerThread = std::thread(&ScannerModule2::worker, this);
    }

    void stop() {
        if (!running) { return; }
        running = false;
        if (workerThread.joinable()) {
            workerThread.join();
        }
        
        if(status_record == true) {
            _recording = false;
            flog::warn("stop() STOP Receiving!");
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_STOP, NULL, NULL);
        }
        core::modComManager.callInterface("Запис", MAIN_SET_STOP, NULL, NULL);

        //logfile.close();
        if(curr_nameWavFile!="")                            
            curlPOST_end(curr_nameWavFile);

        if(curl)
            curl_easy_cleanup(curl);
    }

    std::string genWavFileName(const double current,  const int _mode) {
        // {yymmdd}-{uxtime_ms}-{freq}-{band}-{receivername}.wav
        std::string templ = "$y$M$d-$u-$f-$b-$n-$m.wav";
        // Get data
        time_t now = time(0);
        tm* ltm = localtime(&now);
        using namespace std::chrono;
        milliseconds ms = duration_cast< milliseconds >(
            system_clock::now().time_since_epoch()
        );        
        

        double band = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);        


        // Format to string
        char freqStr[128];
        char dayStr[128];
        char monStr[128];
        char yearStr[128];
        char bandStr[128];

        sprintf(freqStr, "%.0lf", current);
        sprintf(dayStr, "%02d", ltm->tm_mday);
        sprintf(monStr, "%02d", ltm->tm_mon + 1);
        sprintf(yearStr, "%02d", ltm->tm_year-100);
        sprintf(bandStr, "%02d", int(band/1000));

        // core::modComManager.callInterface("Airspy", AIRSPY_IFACE_NAME_SER_NOM, NULL, &sernom_name);

        // 230615-1686831173_250-107400000-300-rp10.wav
        // Replace in template
        templ = std::regex_replace(templ, std::regex("\\$y"), yearStr);
        templ = std::regex_replace(templ, std::regex("\\$M"), monStr);
        templ = std::regex_replace(templ, std::regex("\\$d"), dayStr);
        templ = std::regex_replace(templ, std::regex("\\$u"), std::to_string(ms.count()));
        templ = std::regex_replace(templ, std::regex("\\$f"), freqStr);
        templ = std::regex_replace(templ, std::regex("\\$m"), demodModeList[_mode]);
        templ = std::regex_replace(templ, std::regex("\\$b"), bandStr);
        templ = std::regex_replace(templ, std::regex("\\$n"), thisInstance);

        return templ;
    }   

    bool curlPOST_begin(std::string fname) {
    //                        CURL *curl = curl_easy_init();  
        if(status_direction==false) 
            return false;
        if(curl) {
                                /// const char *url = "http://localhost:18101/event/begin";
            std::string url = thisURL + "/begin";
            char curlErrorBuffer[CURL_ERROR_SIZE];
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuffer);
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str()); //"http://localhost:18101/event/begin"
            curl_easy_setopt(curl, CURLOPT_POST, 1);
            std::string payload = "fname=" + fname;                                 
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            CURLcode res = curl_easy_perform(curl);
            flog::info("curl -i -X POST  {0} {1}", url, payload.c_str());                                
            if(res != CURLE_OK)
                flog::error("curl_easy_perform() failed: {0}", curl_easy_strerror(res));                                                                
            return true;
        }                                      
        return false;
    }
// log::info("curl -i -X POST  {0} {1}", url, payload.c_str());


    bool curlPOST_end(std::string fname) {
        if(status_direction==false) 
            return false;
                            if(curl) {
                                // const char *url = "http://localhost:18101/event/end";
                                std::string url = thisURL + "/begin";
                                char curlErrorBuffer[CURL_ERROR_SIZE];
                                curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuffer);
                                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                                curl_easy_setopt(curl, CURLOPT_POST, 1);
                                /*
                                double dtm = tm/1000.0;
                                std::ostringstream streamObj3;
                                streamObj3 << std::fixed;
                                // Set precision to 3 digits
                                streamObj3 << std::setprecision(3);
                                //Add double to stream
                                streamObj3 << dtm;
                                // Get string from output string stream
                                std::string strUxtime = streamObj3.str();    
                                // std::cout << "unixtm=" << strObj3.c_str() << '\n';                   
                                // std::string payload = "-d uxtime="+strObj3 +" -d freq="+std::to_string(current)+" -d band=" + std::to_string(curr_bandwidth); 
                                */
                                //std::string payload = "-d fname=" + fname + " -d uxtime=" +strUxtime; 
                                std::string payload = "fname=" + fname + "&uxtime=" + utils::unixTimestamp(); 
                                // std::string payload = "fname=" + fname;
                                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
                                flog::info("curl -i -X POST  {0} {1}", url, payload.c_str());                                

                                CURLcode res = curl_easy_perform(curl);
                                if(res != CURLE_OK)
                                    flog::error("curl_easy_perform() failed: {0}", curl_easy_strerror(res));                                

                                return true;
                            }        
        return false;
    }
//                                flog::info("curl -i -X POST  {0} {1}", "http://localhost:18101/event/end", payload.c_str());

    void worker() {
        // 10Hz scan loop
        auto zirotime = std::chrono::time_point<std::chrono::high_resolution_clock>(std::chrono::seconds(1));
        firstSignalTime = zirotime;    
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            {
                std::lock_guard<std::mutex> lck(scanMtx);
                auto now = std::chrono::high_resolution_clock::now();
                // Enforce tuning
                if (gui::waterfall.selectedVFO.empty()) {
                    running = false;
                    return;
                }
                tuner::normalTuning(gui::waterfall.selectedVFO, current);

                // Check if we are waiting for a tune
                if (tuning) {
                    // flog::warn("Tuning");
                    if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTuneTime)).count() > tuningTime) {
                        tuning = false;
                    }
                    continue;
                }

                // Get FFT data
                int dataWidth = 0;
                float* data = gui::waterfall.acquireLatestFFT(dataWidth);
                if (!data) { continue; }

                // Get gather waterfall data
                double wfCenter = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();
                double wfWidth = gui::waterfall.getViewBandwidth();
                double wfStart = wfCenter - (wfWidth / 2.0);
                double wfEnd = wfCenter + (wfWidth / 2.0);

                // Gather VFO data
                double vfoWidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);

                bool endof_level = false;

                if (_Receiving) {
                    // flog::warn("Receiving");
                    float maxLevel = getMaxLevel(data, current, vfoWidth, dataWidth, wfStart, wfWidth);
                    if (maxLevel >= (level-3)) {
                        endof_level = true;
                        if(firstSignalTime==zirotime) { // _finding==true && 
                            //=============== 
                            curr_bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);    
                            flog::info("TRACE.curr_bandwidth = {0}", curr_bandwidth);
                            int _mode = 0 ;
                                if(_count_Bookmark<32000) {
                                    FindedBookmark addFreq;
                                    addFreq.frequency = current;
                                    addFreq.level = ceil(maxLevel);
                                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &_mode);
                                    addFreq.mode = _mode;
                                    addFreq.bandwidth = curr_bandwidth;
                                    addFreq.selected =false;
                                    itFindedFreq = listmaxLevel.begin();
                                    addFreq.left_freq = itFindedFreq->first;
                                    itFindedFreq = listmaxLevel.end();
                                    addFreq.right_freq = itFindedFreq->first;
                                    finded_freq[current] = addFreq;    
                                    _count_Bookmark++;
                                }    
                                
                                flog::info("TRACE. Receiving... ! Current = {0} (finding as {1}). level = {2}, maxLevel = {3}!", current, _count_Bookmark, level, maxLevel);

                            flog::info("TRACE. Receiving... level = {0}, maxLevel = {1}, current = {2}, _lingerTime {3}, _recording={4} !", level, maxLevel, current, _lingerTime, _recording);                                                        
                            firstSignalTime = now;
                            // std::string vfoName = "Канал приймання";
                            curr_nameWavFile = genWavFileName(current, _mode);
                            if(curr_nameWavFile!="")
                                curlPOST_begin(curr_nameWavFile);

                            if(status_record==true && running == true && _recording==false){ 
                                flog::info("TRACE. START Recording!");
                                int recMode = 1;// RECORDER_MODE_AUDIO;
                                core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                                core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_START, (void *) curr_nameWavFile.c_str(), NULL);
                                _recording = true;
                                
                            } else {
                                // flog::info("TRACE. level = {0}, maxLevel = {1}, current = {2} !", level, maxLevel, current);
                            }  
                            // unsigned int tm = std::chrono::duration_cast<std::chrono::milliseconds>(now - zirotime).count();  
                            std::string mylog = getNow() + ". Freq=" + std::to_string(current) + ", bandwidth= "+ std::to_string(curr_bandwidth)+", level=" + std::to_string(maxLevel) + "\r\n";                     
                            logfile = std::ofstream(expandedLogPath.c_str(), std::ios::binary | std::ios::app);
                            if(logfile.is_open()){
                                // flog::info("Recording to '{0}'", expandedPath);
                                logfile.write((char*)mylog.c_str(), mylog.size());
                                logfile.close();
                            }
                            else {
                                flog::error("Could not create '{0}'", expandedLogPath);
                            }
                        }
                        lastSignalTime = now;
                    }
                    else {
                        endof_level = false;
                        flog::info(" TRACE. (maxLevel < level)! level = {0}, maxLevel = {1}, current = {2}, _lingerTime {3}, _recording={4} !", level, maxLevel, current, _lingerTime, _recording);
                        if (status_stop) {
                            if((std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSignalTime)).count() > _waitingTime && _recording==false) 
                                _Receiving = false;
                        } else {
                            
                        }
                    }

                    if(_recording==true) {
                        if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - firstSignalTime)).count() > _lingerTime) {
                            flog::info("TRACE. STOP Receiving! Next... level = {0}, maxLevel = {1}, current = {2}, _lingerTime ={3} !", level, maxLevel, current, _lingerTime);                           
                            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_STOP, NULL, NULL);   
                            _recording=false;   
                            if (!status_stop)        
                                _Receiving = false;                          
                        }
                    }

                    if (!status_stop && _Receiving == true) {
                        if((std::chrono::duration_cast<std::chrono::milliseconds>(now - firstSignalTime)).count() > _lingerTime) {
                            _Receiving = false;
                            flog::info("TRACE. Next... level = {0}, maxLevel = {1}, current = {2} !", level, maxLevel, current);
                        }
                    }
                    if (_Receiving == false) {
                        if(curr_nameWavFile!="") {
                            curlPOST_end(curr_nameWavFile);
                            curr_nameWavFile ="";
                        }         
                        _finding = false;
                        if(selectedLogicId>0)
                            current = last_current;
                        listmaxLevel.clear();
                        _count_freq = 0;                                                
                    }                    
                }
                else {
                    // flog::warn("Seeking signal");
                    double bottomLimit = current;
                    double topLimit = current;
                    firstSignalTime = zirotime;

                    
                    // Search for a signal in scan direction
                    if (findSignalMy(scanUp, bottomLimit, topLimit, wfStart, wfEnd, wfWidth, vfoWidth, data, dataWidth)) {
                        gui::waterfall.releaseLatestFFT();
                        continue;
                    }
                    // Search for signal in the inverse scan direction if direction isn't enforced
                    if (!reverseLock) {
                        if (findSignalMy(!scanUp, bottomLimit, topLimit, wfStart, wfEnd, wfWidth, vfoWidth, data, dataWidth)) {
                            gui::waterfall.releaseLatestFFT();
                            continue;
                        }
                    }
                    else { reverseLock = false; }

                    curr_bandwidth = 0;
                    
                    // There is no signal on the visible spectrum, tune in scan direction and retry
                    if (scanUp) {
                        current = topLimit + interval;
                        if (current > stopFreq) { 
                            if(_count_list>0) { //  || selectedLogicId==0
                                _curr_list++;
                                if(_curr_list>_count_list)
                                    _curr_list = 0;
                                int curr = 0;
                                for (const auto& [bmName, bm] : working_list) {
                                    if(_curr_list==curr) {
                                        loadByName(bmName);
                                        config.acquire();
                                        config.conf["selectedList"] = bmName;
                                        config.release(true);  
                                        if (core::modComManager.interfaceExists(gui::waterfall.selectedVFO)) {
                                        //    if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio") {
                                                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                                                usleep(1000);
                                                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                                                usleep(1000);
                                                float band;
                                                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_BANDWIDTH, NULL, &band);
                                        //    }
                                        }                                                                          
                                        break;
                                    }
                                    curr++;
                                }                            
                            }
                            current = startFreq;                             
                        }
                        flog::info("TRACE. current {0}, interval{1}, topLimit{2}, stopFreq{3}!", current, interval, topLimit, stopFreq);
                    }
                    else {
                        current = bottomLimit - interval;
                        if (current < startFreq) { current = stopFreq; }
                    }

                   // If the new current frequency is outside the visible bandwidth, wait for retune
                    if (current - (vfoWidth/2.0) < wfStart || current + (vfoWidth/2.0) > wfEnd) {
                        lastTuneTime = now;
                        tuning = true;
                    }

                   flog::info("TRACE. current {0}, interval{1}, topLimit{2}!", current, interval, topLimit);

                }

                // Release FFT Data
                gui::waterfall.releaseLatestFFT();
            }
        }
    }

        bool findSignal(bool scanDir, double& bottomLimit, double& topLimit, double wfStart, double wfEnd, double wfWidth, double vfoWidth, float* data, int dataWidth) {
        bool found = false;
        double freq = current;
        for (freq += scanDir ? interval : -interval;
            scanDir ? (freq <= stopFreq) : (freq >= startFreq);
            freq += scanDir ? interval : -interval) {

            // Check if signal is within bounds
            if (freq - (vfoWidth/2.0) < wfStart) { break; }
            if (freq + (vfoWidth/2.0) > wfEnd) { break; }

            if (freq < bottomLimit) { bottomLimit = freq; }
            if (freq > topLimit) { topLimit = freq; }
            
            // Check signal level
            float maxLevel = getMaxLevel(data, freq, vfoWidth * (passbandRatio * 0.01f), dataWidth, wfStart, wfWidth);
            if (maxLevel >= (level-3)) {
                found = true;
                _Receiving = true;
                current = freq;
                break;
            }
        }
        return found;
    }

    #define minFMfreq  80000000
    #define maxFMfreq 110000000
     
    double RoundToMode(double _freq) {
        // 82500000
        if(_freq>minFMfreq && _freq<maxFMfreq) {
            _freq = floor((_freq/100000)+0.5)*100000;
        }
        return _freq;                
    }

    bool findSignalMy(bool scanDir, double& bottomLimit, double& topLimit, double wfStart, double wfEnd, double wfWidth, double vfoWidth, float* data, int dataWidth) {
        bool found = false;
        double freq = current;
//        if(_recording==true)
//            return true;
        double next_step = interval;
        /*
        if(curr_bandwidth>0) {
            if((curr_bandwidth/2)>interval) {
                 next_step =  ceil(interval); //ceil(curr_bandwidth/2); // + interval/2; 
            }
           flog::info("    TRACE. curr_bandwidth={0}, interval={1} , step = {2}!", curr_bandwidth, interval, next_step);
        }
        */
        for (freq += scanDir ? next_step : -next_step;
            scanDir ? (freq <= stopFreq) : (freq >= startFreq);
            freq += scanDir ? next_step : -next_step) {

            // Check if signal is within bounds
            if (freq - (vfoWidth/2.0) < wfStart) { break; }
            if (freq + (vfoWidth/2.0) > wfEnd) { break; }

            if (freq < bottomLimit) { bottomLimit = freq; }
            if (freq > topLimit) { topLimit = freq; }
            freq = (double )ceil(freq);
            // Check signal level

                
            float maxLevel = getMaxLevel(data, freq, vfoWidth * (passbandRatio * 0.01f), dataWidth, wfStart, wfWidth);
            flog::info("TRACE. freq {0}, next_step {1}, maxLevel {2}, level {3}, selectedLogicId = {4}!", freq, next_step, maxLevel, level, selectedLogicId);
            bool endof_level = false;
                        
            if(selectedLogicId==0 || selectedLogicId==1) {
                //=============================================================================
                if (maxLevel >= level) {
                    flog::info("TRACE. maxLevel - level {0} - {1}!", maxLevel, level);
                    if(_privFreq[0]!=freq && _privFreq[1]!=freq) {
                        if(status_ignor) {
                            auto result_find = finded_freq.find(freq);                        
                            if(result_find==finded_freq.end()) {
                                double left_freq  = freq - interval;   
                                double right_freq = freq + interval; 

                                found = true;

                                for (const auto& [key, value] : finded_freq) {
                                    //  flog::info("  TRACE. key={}, value={}", key, value);                      
                                    if(key>left_freq && key<right_freq) {
                                        // flog::info("  TRACE SKIP (by in interval). curr_freq={}, curr_bandwith={}! new freq={}, new bandwidth={}! ", key, value, freq, curr_bandwidth);                      
                                        found = false;
                                        break;
                                    }    
                                }    
                                if(found==true) {
                                    current = freq;
                                    _Receiving = true;
                                    _privFreq[0] = _privFreq[1];
                                    _privFreq[1] = _privFreq[2];
                                    _privFreq[2] = current; 
                                }
                                break;
                            } else {
                                flog::info("TRACE. SKIP... ! Current = {0}!  level = {1}, maxLevel = {2},  !", current, level, maxLevel);
                                _Receiving=false;
                            }
                        } else {
                            _privFreq[0] = _privFreq[1];
                            _privFreq[1] = _privFreq[2];
                            _privFreq[2] = current; 

                            found = true;
                            _Receiving = true;
                            current = freq;
                            break;
                        }               
                    }  
                }
            } else if(selectedLogicId==2) {
            //=============================================================================
            if (maxLevel >= level) {
                endof_level = true;
                FindedFreq currFreq;
                currFreq.frequency = freq;
                currFreq.level = maxLevel;
                listmaxLevel[freq] = currFreq;    
                flog::info(" TRACE.  {0}. curr_freq = {1}, maxLevel {2} >= level ({3})!", _count_freq, freq, maxLevel, level);
                _count_freq++; 
                _Receiving = false;                   
            } else {
                endof_level = false;
                _finding = false;
                flog::info(" TRACE.  {0}. curr_freq = {1}, maxLevel {2} <= (level-3) ({3})!", _count_freq, freq, maxLevel, (level-3));                
                _privFreq[0] = _privFreq[1];
                _privFreq[1] = _privFreq[2];
                _privFreq[2] = current;                 
            }
            
            if(_count_freq>0 && (endof_level == false || _count_freq == 20)) {                        
                flog::info(" TRACE. _count_freq  {0}, endof_level = {1}! listmaxLevel.size() = {2}", _count_freq, endof_level, listmaxLevel.size());

                float _prev = -150;
                double _f_freq = 0.0;
                double _prev_freq = 0.0, _prev_freq_start =0;
                int _cnt_change = 0;
                last_current = freq;
                if(listmaxLevel.size()<3) {
                    _finding = true;
                    for (const auto& [key, lfreq] : listmaxLevel) {
                        _f_freq = key;
                        _prev = lfreq.level;  
                        _prev_freq = key;
                        _prev_freq_start = key;      
                        flog::info("TRACE. _finding -1! _freq = {0}/ {1} .  lfreq.level {2} < _prev = {3}, _cnt_change {4}, _prev_freq_start {5}!", _f_freq, _prev_freq, lfreq.level, _prev, _cnt_change, _prev_freq_start);
                        _f_freq = RoundToMode(_f_freq);
                        flog::info("TRACE. _finding 0! _freq = {0}/ {1} .  lfreq.level {2} < _prev = {3}, _cnt_change {4}, _prev_freq_start {5}!", _f_freq, _prev_freq, lfreq.level, _prev, _cnt_change, _prev_freq_start);
                        break;
                    }    
                    listmaxLevel.clear();
                    FindedFreq currFreq;
                    currFreq.frequency = _f_freq;
                    currFreq.level = _prev;
                    listmaxLevel[freq] = currFreq;    
                }
                if(_finding==false) {
                    for (const auto& [key, lfreq] : listmaxLevel) {
                        flog::info("__TRACE. key = {0}.  lfreq.level {1}!", key, lfreq.level);
                        if( lfreq.level<(_prev-0.5)) {
                            _finding = true;
                            if(_prev_freq!=0) {
                                _f_freq = _prev_freq;                             
                            } else {
                                _f_freq = key;
                            }

                            if(_cnt_change>2) {
                                _f_freq = _prev_freq_start + (_cnt_change/2*next_step);
                            }
                            _f_freq = RoundToMode(_f_freq);
                            flog::info("TRACE. _finding 1! _freq = {0}/ {1} .  lfreq.level {2} < _prev = {3}, _cnt_change {4}, _prev_freq_start {5}!", _f_freq, _prev_freq, lfreq.level, _prev, _cnt_change, _prev_freq_start);

                            break;
                        }
                        if(lfreq.level!=_prev) {
                            _prev = lfreq.level;  
                            _prev_freq_start = key;
                            _cnt_change = 0;
                        } else {
                            _cnt_change++;
                        }
                        _prev_freq = key;
                    } 
                }
                            
                if(_finding==false) {
                    int midl= floor(listmaxLevel.size()/2);
                    int index =0;
                                
                    for (const auto& [key, lfreq] : listmaxLevel) {
                        if (midl==index) {
                            _finding = true;
                            _f_freq = key;
                            _f_freq = RoundToMode(_f_freq);
                            flog::info("TRACE. _finding 2! _freq = {0}/{1}.  lfreq.level {1}, last_current = {2}, index {3}!", _f_freq, key, lfreq.level, last_current, index);                                        
                            break;                                        
                        } 
                        index++;
                    }   
                            
                }
                
                if(_finding) {
                    found = true;
                    for (const auto& [key, value] : finded_freq) {
                        // _f_freq==key || (
                        if(_f_freq==key || (_f_freq>=value.left_freq && _f_freq<=value.right_freq)) {
                            //flog::info("  TRACE SKIP (by in interval). curr_freq={}, curr_bandwith={}! new freq={}, new bandwidth={}! ", key, value, freq, curr_bandwidth);                      
                            flog::info("TRACE SKIP _f_freq {0}, key {1}, value.left_freq {2}, value.right_freq {3}", _f_freq, key, value.left_freq, value.right_freq);
                            found = false;
                            break;
                        }    
                    }    

                    if(found == true) {
                        _Receiving = true;
                        found = true;
                        current = _f_freq;
                        flog::info("TRACE. current = {0}!", current);                                        

                        tuner::normalTuning(gui::waterfall.selectedVFO, current);   
                        break;             
                    } else {
                        _finding = false;
                        listmaxLevel.clear();
                        _count_freq = 0;                                                
                    }
                }    
            }
          }
          usleep(100);   
        }
        return found;
    }

    float getMaxLevel(float* data, double freq, double width, int dataWidth, double wfStart, double wfWidth) {
        double low = freq - (width/2.0);
        double high = freq + (width/2.0);
        int lowId = std::clamp<int>((low - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        int highId = std::clamp<int>((high - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        float max = -INFINITY;
        for (int i = lowId; i <= highId; i++) {
            if (data[i] > max) { max = data[i]; }
        }
        return max;
    }

//=================================================================================================
    void refreshLists() {
        listNames.clear();
        listNamesTxt = "";

        config.acquire();
        for (auto [_name, list] : config.conf["lists"].items()) {
            listNames.push_back(_name);
            listNamesTxt += _name;
            listNamesTxt += '\0';
        }
        config.release();
    }

    void loadFirst() {
        if (listNames.size() > 0) {
            loadByName(listNames[0]);
            return;
        }
        selectedListName = "";
        selectedListId = 0;
    }

    void loadByName(std::string listName) {
        book.clear();
        if (std::find(listNames.begin(), listNames.end(), listName) == listNames.end()) {
            selectedListName = "";
            selectedListId = 0;
            loadFirst();
            return;
        }
        selectedListId = std::distance(listNames.begin(), std::find(listNames.begin(), listNames.end(), listName));
        selectedListName = listName;
        config.acquire();
        for (auto [bmName, bm] : config.conf["lists"].items()) {
            flog::info("!!!! bmName = {0},  bm[listName] = {1} ", bmName, bm["listName"]);

            SearchMode fbm;              
            fbm.listName = bm["listName"];                                  
            fbm._interval = bm["_interval"];            
            fbm._level = bm["_level"];
            fbm._lingerTime = bm["_lingerTime"];
            fbm._passbandRatio = bm["_passbandRatio"];
            fbm._startFreq = bm["_startFreq"];
            fbm._stopFreq = bm["_stopFreq"];
            try {
                fbm._mode = bm["_mode"];
            } catch (...) {
                fbm._mode = 1;
            }
            try {
                fbm._bandwidth = bm["_bandwidth"];
            } catch (...) {
                fbm._bandwidth = 220000;
            }


            if(bm["_status_record"]=="true")
                fbm._status_record = true;
            else     
                fbm._status_record = false;

            if(bm["_status_ignor"]=="true")
                fbm._status_ignor = true;
            else     
                fbm._status_ignor = false;
            
            if(bm["_status_stop"]=="true")
                fbm._status_stop = true;
            else     
                fbm._status_stop = false;

            fbm._tuningTime = bm["_tuningTime"];
            fbm._waitingTime = bm["_waitingTime"];
            fbm.selected = false;            

            // fbm.listName = bm["listName"];                                  
            if(bmName==selectedListName) {
                try {
                    mode             = config.conf["lists"][selectedListName]["_mode"];
                } catch (...) {
                    mode = 1;
                }
                try {
                    bandwidth        = config.conf["lists"][selectedListName]["_bandwidth"];
                } catch (...) {
                    bandwidth = 220000;
                }
                for (int i = 0; i < bandwidthsList.size(); i++) {
                    if (bandwidthsList[i] >= bandwidth) {
                        _bandwidthId = i;
                        break;
                    }
                }
                interval         = config.conf["lists"][selectedListName]["_interval"];            
                level            = config.conf["lists"][selectedListName]["_level"];
                intLevel = level;
                _lingerTime      = config.conf["lists"][selectedListName]["_lingerTime"];
                passbandRatio    = config.conf["lists"][selectedListName]["_passbandRatio"];
                startFreq        = config.conf["lists"][selectedListName]["_startFreq"];
                stopFreq         = config.conf["lists"][selectedListName]["_stopFreq"];
                status_record    = fbm._status_record;
                status_ignor     = fbm._status_ignor;
                status_stop      = fbm._status_stop ;
                tuningTime       = config.conf["lists"][selectedListName]["_tuningTime"];
                _waitingTime     = config.conf["lists"][selectedListName]["_waitingTime"];
                selectedIntervalId = 0;
                for (int i = 0; i < intervalsList.size(); i++) {
                    if (intervalsList[i] == interval) {
                        selectedIntervalId = i;
                        break;
                    }
                }

            }

            book[bmName] = fbm;

           // flog::info("!!!! bmName = {0},  status_record = {1}, fbm[status_record] ={2} ", bmName, status_record, fbm._status_record);
        }
        config.release();
    }

    bool newListDialog() {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "New##scann3_new_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[1024];
        strcpy(nameBuf, editedListName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            ImGui::LeftLabel("Назва");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputText(("##scann3_edit_name_3" + name).c_str(), nameBuf, 1023)) {
                editedListName = nameBuf;
            }

            bool alreadyExists = (std::find(listNames.begin(), listNames.end(), editedListName) != listNames.end());
            if (renameListOpen) 
                    alreadyExists = false;

            if(strlen(nameBuf) == 0)
                alreadyExists =true;

            if (alreadyExists) { ImGui::BeginDisabled(); }
            if (ImGui::Button("OK")) {
                open = false;
              flog::info("!!!! status_record = {0} ", status_record);

                config.acquire();
                if (renameListOpen) {
//                    config.conf["lists"][editedListName] = config.conf["lists"][firstEditedListName];
                    config.conf["lists"].erase(firstEditedListName);
                    json def;
                    def = json::object();
                    def["listName"]= editedListName;
                    def["_mode"]= mode;
                    def["_bandwidth"]= bandwidth;
                    def["_startFreq"]= startFreq;
                    def["_stopFreq"]=  stopFreq;
                    def["_interval"]= interval;
                    def["_passbandRatio"]= passbandRatio;
                    def["_tuningTime"]= tuningTime;
                    def["_waitingTime"]= _waitingTime;
                    def["_lingerTime"]= _lingerTime;
                    def["_level"]= level;

                    if(status_stop==true)
                        def["_status_stop"]= "true";
                    else     
                        def["_status_stop"]= "false";

                    if(status_record==true)
                        def["_status_record"]= "true";
                    else     
                        def["_status_record"]= "false";

                    if(status_ignor==true)
                        def["_status_ignor"]= "true";
                    else     
                        def["_status_ignor"]= "false";

                    config.conf["lists"][editedListName] = true;
                    config.conf["lists"][editedListName] =  def;

                }
                else {
                    json def;
                    def = json::object();
                    def["listName"]= editedListName;
                    def["_mode"]= mode;
                    def["_bandwidth"]= bandwidth;
                    def["_startFreq"]= startFreq;
                    def["_stopFreq"]=  stopFreq;
                    def["_interval"]= interval;
                    def["_passbandRatio"]= passbandRatio;
                    def["_tuningTime"]= tuningTime;
                    def["_waitingTime"]= _waitingTime;
                    def["_lingerTime"]= _lingerTime;
                    def["_level"]= level;

                    if(status_stop==true)
                        def["_status_stop"]= "true";
                    else     
                        def["_status_stop"]= "false";

                    if(status_record==true)
                        def["_status_record"]= "true";
                    else     
                        def["_status_record"]= "false";

                    if(status_ignor==true)
                        def["_status_ignor"]= "true";
                    else     
                        def["_status_ignor"]= "false";

                    config.conf["lists"][editedListName] = true;
                    config.conf["lists"][editedListName] =  def;
                    
                }
                config.release(true);
                refreshLists();
                loadByName(editedListName);
            }

            if (alreadyExists) { ImGui::EndDisabled(); }
            ImGui::SameLine();
            if (ImGui::Button("Скасувати")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }
//==========================================================================================

    std::string expandString(std::string input) {
        input = std::regex_replace(input, std::regex("%ROOT%"), root);
        return std::regex_replace(input, std::regex("//"), "/");
    }

    std::string name;
    bool enabled = true;
    
    bool running = false;
    //std::string selectedVFO = "Канал приймання";
    int mode = 1;
    double startFreq = 100000000.0;
    double stopFreq = 180000000.0;
//    double interval = 1000.0;
    unsigned int interval = 1000;
    double current = startFreq;
    double last_current = startFreq;
    double _privFreq[3] = {0,0,0};
    double passbandRatio = 10.0;
    int tuningTime = 350;
    float level = -70.0;
    int intLevel = -70;
    bool _Receiving = false;
    bool tuning = false;
    bool scanUp = true;
    bool reverseLock = false;
    bool status_record = true;
    int _lingerTime = 3000;
    int _waitingTime = 1000;
    
    bool _recording = false;
    bool status_stop = false;
    bool status_ignor = true;
    unsigned int _count_freq = 0;
    unsigned int _count_Bookmark =0;
    double curr_bandwidth = 0;
//    std::vector<double, double> finded_freq; 
    std::map<double, FindedBookmark> finded_freq; 
    std::map<double, FindedFreq> listmaxLevel;  
    std::map<double, FindedFreq>::iterator itFindedFreq;  
    bool _finding =false;

    std::ofstream logfile;
    std::string expandedLogPath;
    std::string selectedRecorder = "";

    std::chrono::time_point<std::chrono::high_resolution_clock> lastSignalTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> lastTuneTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> firstSignalTime;

    std::thread workerThread;
    std::mutex scanMtx;

    std::vector<std::string> listNames;
    bool deleteListOpen = false;
    int selectedListId = 0;
    std::string selectedListName = "";
    std::string listNamesTxt = "";
    std::map<std::string, SearchMode> book;
    std::map<std::string, SearchMode>::iterator itbook;
    std::string editedListName;
    std::string firstEditedListName;
    bool renameListOpen = false;
    bool newListOpen = false;

    std::vector<uint32_t> intervalsList;
    std::string intervalsListTxt;
    int selectedIntervalId=0;

    std::vector<std::string>  logicList;
    std::string logicListTxt;
    int selectedLogicId  = 0;
    bool status_direction = false;

    std::map<std::string, SearchMode> working_list; 
    int _count_list = 0;
    int _curr_list = 0; 

    std::string root = (std::string)core::args["root"];
    std::string curr_nameWavFile = "";

    float bandwidth = 220000;
    std::vector<uint32_t> bandwidthsList;
    int _frec = 0, _bandwidthId = 6;
    bool _raw = false;

    std::string thisURL = "http://localhost:18101/event/";
    std::string thisInstance = "test";

    CURL *curl;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["selectedList"] = "General";
    def["lists"]["General"] = json::object();
    def["lists"]["General"]["listName"]= "General";
    def["lists"]["General"]["_mode"]= 1;
    def["lists"]["General"]["_bandwidth"]= 6;
    def["lists"]["General"]["_startFreq"]= 100000000.0;
    def["lists"]["General"]["_stopFreq"]= 120000000.0;
    def["lists"]["General"]["_interval"]= 10000.0;
    def["lists"]["General"]["_passbandRatio"]= 10.0;
    def["lists"]["General"]["_tuningTime"]= 350;
    def["lists"]["General"]["_status_stop"]= "false";
    def["lists"]["General"]["_waitingTime"]= 1000;
    def["lists"]["General"]["_status_record"]= "true";
    def["lists"]["General"]["_lingerTime"]= 3000;
    def["lists"]["General"]["_status_ignor"]= "true";
    def["lists"]["General"]["_level"]= -70;

    config.setPath(core::args["root"].s() + "/search.json");
    config.load(def);
    config.enableAutoSave();

    // Check if of list and convert if they're the old type
    config.acquire();
   
    for (auto [listName, list] : config.conf["lists"].items()) {
//        if (list.contains("bookmarks") && list.contains("showOnWaterfall") && list["showOnWaterfall"].is_boolean()) { continue; }
        json newList;
        newList = json::object();
//        newList["showOnWaterfall"] = true;
        newList[listName] = list;
//        config.conf[listName] = newList;
        std::string name = config.conf["lists"][listName]["listName"];
//        flog::info(" listName = {0},  listName = {1} ", listName, name.c_str());
    }

    config.release(true);    
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ScannerModule2(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (ScannerModule2*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();    
}