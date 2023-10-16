#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <thread>
#include <radio_interface.h>
#include <signal_path/signal_path.h>
#include <vector>
#include <gui/tuner.h>
#include <gui/file_dialogs.h>
#include <utils/freq_formatting.h>
#include <gui/dialogs/dialog_box.h>
#include <fstream>

#include <core.h>
#include <ctime>
#include <chrono>

#include <curl/curl.h>

SDRPP_MOD_INFO{
    /* Name:            */ "scanner3",
    /* Description:     */ "Observation manager module for Aster",
    /* Author:          */ "DMH",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ 1
};

struct ObservationBookmark {
    double frequency;
    float bandwidth;
    int mode;
    int level;
    bool selected;
};

struct WaterfallBookmark {
    std::string listName;
    std::string bookmarkName;
    ObservationBookmark bookmark;
};

ConfigManager config;

// const char* bandListTxt = " 1000\0 6250\0 12500\0 25000\0 50000\0 100000\0 220000\0";

enum {
    BOOKMARK_DISP_MODE_OFF,
    BOOKMARK_DISP_MODE_TOP,
    BOOKMARK_DISP_MODE_BOTTOM,
    _BOOKMARK_DISP_MODE_COUNT
};
/**/
enum {
    RECORDER_IFACE_CMD_GET_MODE,
    RECORDER_IFACE_CMD_SET_MODE,
    RECORDER_IFACE_CMD_START,
    RECORDER_IFACE_CMD_STOP,
    RECORDER_IFACE_CMD_SET_STREAM,
    MAIN_SET_START,
    MAIN_SET_STOP,
    MAIN_GET_PROCESSING
};


const char* bookmarkDisplayModesTxt = "Off\0Top\0Bottom\0";

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


class ObservationManagerModule : public ModuleManager::Instance {
public:
    ObservationManagerModule(std::string name) {
        this->name = name;

        config.acquire();
        std::string selList = config.conf["selectedList"];
        curr_listName = selList;
        bookmarkDisplayMode = config.conf["bookmarkDisplayMode"];
        config.release();

        refreshLists();
        loadByName(selList);
        refreshWaterfallBookmarks(true);

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        inputHandler.ctx = this;
        inputHandler.handler = fftInput;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.bindHandler(&inputHandler);

        root = (std::string)core::args["root"];        
        flog::info("name={0}", name);

        thisURL = core::configManager.conf["Url"];
        thisInstance = core::configManager.conf["InstanceName"];
        thisInstance = thisInstance + "-3";


        bandwidthsList.clear();
        bandwidthsList.push_back(1000);
        bandwidthsList.push_back(6250);
        bandwidthsList.push_back(12500);
        bandwidthsList.push_back(25000);
        bandwidthsList.push_back(50000);
        bandwidthsList.push_back(100000);
        bandwidthsList.push_back(220000);

        core::modComManager.registerInterface("scanner3", name, moduleInterfaceHandler, this);
    }

    ~ObservationManagerModule() {
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.unbindHandler(&inputHandler);
        core::modComManager.unregisterInterface(name);
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
    static void applyMode(ObservationBookmark bm, std::string vfoName) {
        if (vfoName != "") {
            if (core::modComManager.interfaceExists(vfoName)) {
                if (core::modComManager.getModuleName(vfoName) == "radio") {
                    int mode = bm.mode;
                    float bandwidth = bm.bandwidth;
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                }
            }            
//            tuner::normalTuning(gui::waterfall.selectedVFO, bm.frequency);
        } else {
            gui::waterfall.setCenterFrequency(bm.frequency);
            gui::waterfall.centerFreqMoved = true;
        }
    }

    static void applyBookmark(ObservationBookmark bm, std::string vfoName) {
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

    bool bookmarkEditDialog() {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        std::string id = "Edit##freq_manager_edit_popup_3" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[1024];
        strcpy(nameBuf, editedBookmarkName.c_str());
                
        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            ImGui::BeginTable(("freq_manager_edit_table" + name).c_str(), 2);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Назва");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(150);
            if (ImGui::InputText(("##freq_manager_edit_name_3" + name).c_str(), nameBuf, 1023)) {
                editedBookmarkName = nameBuf;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Частота, Гц");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(150);
//            ImGui::InputDouble(("##freq_manager_edit_freq_3" + name).c_str(), &editedBookmark.frequency);
            ImGui::InputInt(("##freq_manager_edit_freq_3" + name).c_str(), &_frec, 100, 100000);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Смуга, Гц");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(150);
            // ImGui::InputDouble(("##freq_manager_edit_bw_3" + name).c_str(), &editedBookmark.bandwidth);
            
            if (ImGui::Combo(("##freq_manager_edit_bw_3" + name).c_str(), &_bandwidthId, bandListTxt)) {
                editedBookmark.bandwidth = bandwidthsList[_bandwidthId];

                flog::info("TRACE. editedBookmark.bandwidth = {0} !", editedBookmark.bandwidth);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Режим");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(150);
            // ImGui::Combo(("##freq_manager_edit_mode_3" + name).c_str(), &editedBookmark.mode, demodModeListTxt);
            if (ImGui::Combo(("##freq_manager_edit_mode_3" + name).c_str(), &editedBookmark.mode, demodModeListTxt)) {
                editedBookmark.mode = editedBookmark.mode;
                if(editedBookmark.mode==7) {
                    _raw = true;
                    editedBookmark.bandwidth = 220000;
                    _bandwidthId = 6;
                } else {
                    _raw =false;
                }       
                flog::info("TRACE. editedBookmark.mode = {0}, getSampleRate() {1} !", editedBookmark.mode, sigpath::iqFrontEnd.getSampleRate());
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Поріг виявлення, дБ");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(150);
            ImGui::InputInt(("##freq_manager_edit_level_3" + name).c_str(), &editedBookmark.level, -150, 0);

            ImGui::EndTable();

            bool applyDisabled = (strlen(nameBuf) == 0) || _raw ==true || (bookmarks.find(editedBookmarkName) != bookmarks.end() && editedBookmarkName != firstEditedBookmarkName);

            if(applyDisabled==false  && editOpen==false) {
                for (auto& [name, bm] : bookmarks) {
                    editedBookmark.frequency = _frec;
                    if(bm.frequency==editedBookmark.frequency && bm.mode==editedBookmark.mode) {
                        applyDisabled = true;
                        break;
                    }    
                }        
            }

            if (applyDisabled) { style::beginDisabled(); }
            if (ImGui::Button("OK")) {
                open = false;
        
                editedBookmark.frequency = _frec;

                // If editing, delete the original one
                if (editOpen) {
                    bookmarks.erase(firstEditedBookmarkName);
                }
                bookmarks[editedBookmarkName] = editedBookmark;

                saveByName(selectedListName);
                
                ObservationBookmark& bm = bookmarks[editedBookmarkName];
                applyBookmark(bm, gui::waterfall.selectedVFO);
                bm.selected = false;
            }
            if (applyDisabled) { style::endDisabled(); }
            ImGui::SameLine();
            if (ImGui::Button("Скасувати")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    bool newListDialog() {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "New##freq_manager_new_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[1024];
        strcpy(nameBuf, editedListName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            ImGui::LeftLabel("Назва");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputText(("##freq_manager_edit_name_3" + name).c_str(), nameBuf, 1023)) {
                editedListName = nameBuf;
            }

            bool alreadyExists = (std::find(listNames.begin(), listNames.end(), editedListName) != listNames.end());

            if (strlen(nameBuf) == 0 || alreadyExists) { style::beginDisabled(); }
            if (ImGui::Button("OK")) {
                open = false;
                waterfallBookmarks.clear();

                config.acquire();
                if (renameListOpen) {
                    config.conf["lists"][editedListName] = config.conf["lists"][firstEditedListName];
                    config.conf["lists"].erase(firstEditedListName);
                }
                else {
                    config.conf["lists"][editedListName]["showOnWaterfall"] = true;
                    config.conf["lists"][editedListName]["bookmarks"] = json::object();
                }
                refreshWaterfallBookmarks(false);
                config.release(true);
                refreshLists();
                loadByName(editedListName);
                curr_listName = editedListName;
            }
            if (strlen(nameBuf) == 0 || alreadyExists) { style::endDisabled(); }
            ImGui::SameLine();
            if (ImGui::Button("Скасувати")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    bool selectListsDialog() {
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "Вибір Банку##freq_manager_sel_popup_3" + name;
        ImGui::OpenPopup(id.c_str());

        bool open = true;

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            // No need to lock config since we're not modifying anything and there's only one instance
            for (auto [listName, list] : config.conf["lists"].items()) {
                bool shown = list["showOnWaterfall"];
                config.conf["lists"][listName]["showOnWaterfall"] = false;
                if (ImGui::Checkbox((listName + "##freq_manager_sel_list_3").c_str(), &shown)) {
                    config.acquire();
                    config.conf["lists"][listName]["showOnWaterfall"] = shown;
                    refreshWaterfallBookmarks(false);
                    config.release(true);
                    curr_listName = listName;
                }
            }

            if (ImGui::Button("Ok")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

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

    void refreshWaterfallBookmarks(bool lockConfig = true) {
        if (lockConfig) { config.acquire(); }
        waterfallBookmarks.clear();
        for (auto [listName, list] : config.conf["lists"].items()) {
            if (!((bool)list["showOnWaterfall"])) { continue; }

            if(listName!=curr_listName)
                        continue;
            flog::info("showOnWaterfall ={0}", listName.c_str());                        
            WaterfallBookmark wbm;
            wbm.listName = listName;
            for (auto [bookmarkName, bm] : config.conf["lists"][listName]["bookmarks"].items()) {
                wbm.bookmarkName = bookmarkName;
                wbm.bookmark.frequency = config.conf["lists"][listName]["bookmarks"][bookmarkName]["frequency"];
                wbm.bookmark.bandwidth = config.conf["lists"][listName]["bookmarks"][bookmarkName]["bandwidth"];
                wbm.bookmark.mode = config.conf["lists"][listName]["bookmarks"][bookmarkName]["mode"];
                
                try
                {
                    wbm.bookmark.level = config.conf["lists"][listName]["bookmarks"][bookmarkName]["level"];
                } 
                catch(const std::exception& e)
                {
                    std::cerr << e.what() << '\n';
                    wbm.bookmark.level      = -50;
                }
                
                wbm.bookmark.selected = false;
                waterfallBookmarks.push_back(wbm);
            }
        }
        if (lockConfig) { config.release(); }
    }

    void loadFirst() {
        if (listNames.size() > 0) {
            loadByName(listNames[0]);
            refreshWaterfallBookmarks(false);
            return;
        }
        selectedListName = "";
        selectedListId = 0;
    }

    void loadByName(std::string listName) {
        bookmarks.clear();
        if (std::find(listNames.begin(), listNames.end(), listName) == listNames.end()) {
            selectedListName = "";
            selectedListId = 0;
            loadFirst();
            return;
        }
        selectedListId = std::distance(listNames.begin(), std::find(listNames.begin(), listNames.end(), listName));
        selectedListName = listName;
        curr_listName = listName; 
        config.acquire();
        for (auto [bmName, bm] : config.conf["lists"][listName]["bookmarks"].items()) {
            ObservationBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.mode = bm["mode"];
            try
            {
                fbm.level = bm["level"];
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << '\n';
                fbm.level = -50;
            }
            fbm.selected = false;
            bookmarks[bmName] = fbm;
        }
        config.release();
    }

    void saveByName(std::string listName) {
        config.acquire();
        config.conf["lists"][listName]["bookmarks"] = json::object();
        for (auto [bmName, bm] : bookmarks) {
            config.conf["lists"][listName]["bookmarks"][bmName]["frequency"] = bm.frequency;
            config.conf["lists"][listName]["bookmarks"][bmName]["bandwidth"] = bm.bandwidth;
            config.conf["lists"][listName]["bookmarks"][bmName]["mode"] = bm.mode;
            config.conf["lists"][listName]["bookmarks"][bmName]["level"] = bm.level;
        }
        refreshWaterfallBookmarks(false);
        config.release(true);
    }

    static void menuHandler(void* ctx) {
        int _air_recording;

        ObservationManagerModule* _this = (ObservationManagerModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        int _work;
        core::modComManager.callInterface("Запис", MAIN_GET_PROCESSING, NULL, &_work);
        int _run =_this->running;
        if (_run || _work>0) { ImGui::BeginDisabled(); }

        // TODO: Replace with something that won't iterate every frame
        std::vector<std::string> selectedNames;
        for (auto& [name, bm] : _this->bookmarks) {
            if (bm.selected) { 
            selectedNames.push_back(name); 
            }
        }

        float lineHeight = ImGui::GetTextLineHeightWithSpacing();

        float btnSize = ImGui::CalcTextSize("Зберегти").x + 8;
        ImGui::SetNextItemWidth(menuWidth - 24 - (2 * lineHeight) - btnSize);
        if (ImGui::Combo(("##freq_manager_list_sel_3" + _this->name).c_str(), &_this->selectedListId, _this->listNamesTxt.c_str())) {
            _this->loadByName(_this->listNames[_this->selectedListId]);
            config.acquire();
            config.conf["selectedList"] = _this->selectedListName;
            config.release(true);
            _this->refreshWaterfallBookmarks(false); 
        }
        ImGui::SameLine();
        if (_this->listNames.size() == 0) { style::beginDisabled(); }
        if (ImGui::Button(("Зберегти##_freq_mgr_ren_lst_3" + _this->name).c_str(), ImVec2(btnSize, 0))) {
            _this->firstEditedListName = _this->listNames[_this->selectedListId];
            _this->editedListName = _this->firstEditedListName;
            _this->renameListOpen = true;
        }
        if (_this->listNames.size() == 0) { style::endDisabled(); }
        ImGui::SameLine();
        if (ImGui::Button(("+##_freq_mgr_add_lst_3" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
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
                 // bool _record = true;

            }
            _this->newListOpen = true;
        }
        ImGui::SameLine();
        if (_this->selectedListName == "") { style::beginDisabled(); }
        if (ImGui::Button(("-##_freq_mgr_del_lst_3" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
            _this->deleteListOpen = true;
        }
        if (_this->selectedListName == "") { style::endDisabled(); }

        // List delete confirmation
        if (ImGui::GenericDialog(("freq_manager_del_list_confirm3" + _this->name).c_str(), _this->deleteListOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::Text("Видалення банку \"%s\". Ви впевнені?", _this->selectedListName.c_str());
            }) == GENERIC_DIALOG_BUTTON_YES) {
            config.acquire();
            config.conf["lists"].erase(_this->selectedListName);
            _this->refreshWaterfallBookmarks(false);
            config.release(true);
            _this->refreshLists();
            _this->selectedListId = std::clamp<int>(_this->selectedListId, 0, _this->listNames.size());
            if (_this->listNames.size() > 0) {
                _this->loadByName(_this->listNames[_this->selectedListId]);
            }
            else {
                _this->selectedListName = "";
            }
        }

        if (_this->selectedListName == "") { style::beginDisabled(); }

       //Draw import and export buttons
        ImGui::BeginTable(("freq_manager_bottom_btn_table3" + _this->name).c_str(), 2);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Iмпорт##_freq_mgr_imp_3" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->importOpen) {
            try
            {
                _this->importOpen = true;
                _this->importDialog = new pfd::open_file("Import bookmarks", "", { "JSON Files (*.json)", "*.json", "All Files", "*" }, true);
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << '\n';
            }

        }

        ImGui::TableSetColumnIndex(1);
        // if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Експорт##_freq_mgr_exp_3" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->exportOpen) {
            try
            {
                _this->exportedBookmarks = json::object();
                config.acquire();
                for (auto [bmName, bm] : config.conf["lists"][_this->selectedListName]["bookmarks"].items()) {
                    _this->exportedBookmarks["bookmarks"][bmName] = config.conf["lists"][_this->selectedListName]["bookmarks"][bmName];
                }
                config.release();
                _this->exportOpen = true;
                _this->exportDialog = new pfd::save_file("Export bookmarks", "export2_.json", { "JSON Files (*.json)", "*.json", "All Files", "*" }, true);
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << '\n';
            }
            
        }        
        // if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::EndTable();

        //Draw buttons on top of the list
        ImGui::BeginTable(("freq_manager_btn_table" + _this->name).c_str(), 3);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Додати##_freq_mgr_add_3" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            // If there's no VFO selected, just save the center freq
            if (gui::waterfall.selectedVFO == "") {
                _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency(); 
                _this->editedBookmark.bandwidth = 0;
                _this->editedBookmark.mode = 7;
            }
            else {
                _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
                _this->editedBookmark.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
                _this->editedBookmark.mode = 7;
                if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio") {
                    int mode;
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                    _this->editedBookmark.mode = mode;
                }
            }
            _this->_frec = round(_this->editedBookmark.frequency);
            _this->_bandwidthId = 0;
            _this->_raw=false;
            for (int i = 0; i < _this->bandwidthsList.size(); i++) {
                if (_this->bandwidthsList[i] >= _this->editedBookmark.bandwidth) {
                    _this->_bandwidthId = i;
                    break;
                }
            }

            float maxLevel = _this->level;
            int dataWidth = 0;
            float* data = gui::waterfall.acquireLatestFFT(dataWidth);
            if (data) { 
                // Get gather waterfall data
                double wfCenter = _this->editedBookmark.frequency;
                double wfWidth =_this->editedBookmark.bandwidth;
                double wfStart = wfCenter - (wfWidth / 2.0);
                double wfEnd = wfCenter + (wfWidth / 2.0);
                // Gather VFO data
                double vfoWidth = _this->editedBookmark.bandwidth;

                // Check if we are waiting for a tune                    
                maxLevel = _this->getMaxLevel(data, _this->current, vfoWidth, dataWidth, wfStart, wfWidth) + 5;            
            }    
            // Release FFT Data
            gui::waterfall.releaseLatestFFT();
            // if(maxLevel<-150.0) maxLevel=-150;

            _this->editedBookmark.level = maxLevel;


            _this->editedBookmark.selected = false;

            _this->createOpen = true;

            // Find new unique default name
            if (_this->bookmarks.find("1") == _this->bookmarks.end()) {
                _this->editedBookmarkName = "1";
            }
            else {
                char buf[64];
                for (int i = 2; i < 1000; i++) {
                    sprintf(buf, "%d", i);
                    if (_this->bookmarks.find(buf) == _this->bookmarks.end()) { break; }
                }
                _this->editedBookmarkName = buf;
            }
        }

        ImGui::TableSetColumnIndex(1);
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Видалити##_freq_mgr_rem_3" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->deleteBookmarksOpen = true;
        }
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::TableSetColumnIndex(2);
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Редаг##_freq_mgr_edt_3" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->editOpen = true;
            _this->editedBookmark = _this->bookmarks[selectedNames[0]];
            _this->editedBookmarkName = selectedNames[0];
            _this->firstEditedBookmarkName = selectedNames[0];
            _this->_frec = round(_this->editedBookmark.frequency);
            _this->_bandwidthId = 0;
            _this->_raw=false;
            for (int i = 0; i < _this->bandwidthsList.size(); i++) {
                if (_this->bandwidthsList[i] >= _this->editedBookmark.bandwidth) {
                    _this->_bandwidthId = i;
                    break;
                }
            }
        }
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::endDisabled(); }

        ImGui::EndTable();

        // Bookmark delete confirm dialog
        // List delete confirmation
        if (ImGui::GenericDialog(("freq_manager_del_list_confirm" + _this->name).c_str(), _this->deleteBookmarksOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::TextUnformatted("Видалити відмічену частоту? Ви впевнені?");
            }) == GENERIC_DIALOG_BUTTON_YES) {
            for (auto& _name : selectedNames) { _this->bookmarks.erase(_name); }
            _this->saveByName(_this->selectedListName);
        }
 
        // Bookmark list
        if (ImGui::BeginTable(("freq_manager_bkm_table_3" + _this->name).c_str(), 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 150))) {
            ImGui::TableSetupColumn("Назва");
            ImGui::TableSetupColumn("Частота, МГц");
            ImGui::TableSetupColumn("Смуга, кГц");
            ImGui::TableSetupColumn("Поріг, дБ");
            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableHeadersRow();
            for (auto& [name, bm] : _this->bookmarks) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImVec2 min = ImGui::GetCursorPos();

                if (ImGui::Selectable((name + "##_freq_mgr_bkm_name_3" + _this->name).c_str(), &bm.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick)) {
                    // if shift or control isn't pressed, deselect all others
                    if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) {
                        for (auto& [_name, _bm] : _this->bookmarks) {
                            if (name == _name) { continue; }
                            _bm.selected = false;
                        }
                    }
                }
                if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    applyBookmark(bm, gui::waterfall.selectedVFO);
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s %s", utils::formatFreqMHz(bm.frequency).c_str(), demodModeList[bm.mode]);

                ImGui::TableSetColumnIndex(2);
                int bw = round(bm.bandwidth/1000);
                std::string sbw = std::to_string(bw);
                if(bw==13) sbw ="12.5";
                if(bw==6) sbw ="6.25";

                ImGui::Text("%s",sbw.c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s",std::to_string(bm.level).c_str());

                ImVec2 max = ImGui::GetCursorPos();
            }
            ImGui::EndTable();
        }

        /*
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Зберегти##_freq_mgr_apply_3" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            ObservationBookmark& bm = _this->bookmarks[selectedNames[0]];
            applyBookmark(bm, gui::waterfall.selectedVFO);
            bm.selected = false;
        }

        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::endDisabled(); }
        */

        // if (ImGui::Button(("Спостерігати за частотами банку##_freq_mgr_exp_3" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
        //     _this->selectListsOpen = true;
        // } 

        //-----------------------------------------------------------------
        ImGui::Checkbox("Виставити загальний поріг для всіх частот##_scanner3_porig", &_this->flag_level);

        if (!_this->flag_level) { ImGui::BeginDisabled(); }
        ImGui::LeftLabel("Поріг виявлення, дБ");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        ImGui::SliderFloat("##scanner3_level_3", &_this->level, -150.0, 0.0);
        if (!_this->flag_level) { ImGui::EndDisabled(); }

        //-----------------------------------------------------------------

        ImGui::Checkbox("Призупиняти сканування при виявленні сигналу##_status_scanner3", &_this->status_stop); // status_stop

        //---------------------------------------------------
        if (!_this->status_stop) { ImGui::BeginDisabled(); }
        ImGui::LeftLabel("Макс. час очикування сигналу (мс)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt("##linger_timeWait_scanner_3", &_this->_waitingTime, 100, 1000)) {
            _this->_waitingTime = std::clamp<int>(_this->_waitingTime, 100, 100000.0);
        }
        if (!_this->status_stop) { ImGui::EndDisabled(); }
        //---------------------------------------------------

        ImGui::Checkbox("Реєструвати##_record_2", &_this->_record);

        if (_this->_record) {
            ImGui::LeftLabel("Тривалість запису (мс)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            ImGui::InputInt("##linger_time_scanner_3", &_this->_recordTime, 100, 1000);
        } else {
            ImGui::LeftLabel("Тривалість затримки (мс)");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            ImGui::InputInt("##linger_time_scanner_3_3", &_this->_recordTime, 100, 1000);
        }
//            _this->_lingerTime = std::clamp<int>(_this->_lingerTime, 10, 100000.0);

        if (_run || _work>0 ) { ImGui::EndDisabled(); }

/*
        ImGui::LeftLabel("Тривалість запису (мс)");
        if (ImGui::InputInt("##recording_time_scanner_3", &_this->_recordTime, 100, 1000)) {
            _this->_recordTime = std::clamp<int>(_this->_recordTime, 100, 10000.0);
        }
*/

        core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);

        if (!_this->running && !_this->_detected) {
                if(_work>0)  ImGui::BeginDisabled(); 
                    core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);
                    if(_air_recording==1)  {
                        if (ImGui::Button("СТАРТ##scanner3_start_3", ImVec2(menuWidth, 0))) {
                           _this->start();
                        }
                    } else {
                        style::beginDisabled();
                        ImGui::Button("СТАРТ##scanner3_start_3", ImVec2(menuWidth, 0));
                        style::endDisabled();
                    }    
                if(_work>0)  ImGui::EndDisabled(); 
        }
        else {
            if(_air_recording==0)  {
                _this->stop();
                _this->_detected = false;
            }

            if(_this->_detected==true && _this->_recording == true && _this->status_stop == true) { // && _this->status_stop == true
                if (ImGui::Button("ДАЛІ##scanner3_cans_3", ImVec2(menuWidth, 0))) {
                    _this->stop();
                    _this->_recording = false;
                    _this->_Receiving = false;                    
                    _this->start();
                } 
            } else {
                if (ImGui::Button("СТОП ##scanner3_start_3", ImVec2(menuWidth, 0))) {
                    _this->stop();
                    _this->_detected =false;
                }
            }


            
        }

        if(_this->_detected ) {
            if(_this->_recording == true && _this->status_stop == true ) { // 
                if (ImGui::Button("Зупинити запис##scanner3_recstop_3", ImVec2(menuWidth, 0))) {
                    _this->stop();
                    _this->_recording = false;
                    _this->_Receiving = false;
                    _this->_detected =false;
                }
            } else {
                if (ImGui::Button("ДАЛІ##scanner3_cans_3", ImVec2(menuWidth, 0))) {
                    _this->stop();
                    _this->_recording = false;
                    _this->_Receiving = false;                    
                    _this->start();
                } 
            }
        }
        if (!_this->running && !_this->_detected) {
            ImGui::Text("Статус: Неактивний");
        }  else {
            if(_this->_recording == true) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Статус: Реєстрація");
            } else if (_this->tuning) {
                ImGui::TextColored(ImVec4(0, 1, 1, 1), "Статус: Сканування");
            } else if (_this->_detected) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Статус: Приймання");
            }
            else {
                ImGui::TextColored(ImVec4(0, 1, 1, 1), "Статус: Сканування");
            }            
        }


        if (_this->selectedListName == "") { style::endDisabled(); }

        if (_this->createOpen) {
            _this->createOpen = _this->bookmarkEditDialog();
        }

        if (_this->editOpen) {
            _this->editOpen = _this->bookmarkEditDialog();
        }

        if (_this->newListOpen) {
            _this->newListOpen = _this->newListDialog();
        }

        if (_this->renameListOpen) {
            _this->renameListOpen = _this->newListDialog();
        }

        if (_this->selectListsOpen) {
            _this->selectListsOpen = _this->selectListsDialog();
        }

        // Handle import and export
        if (_this->importOpen && _this->importDialog->ready()) {
            _this->importOpen = false;
            std::vector<std::string> paths = _this->importDialog->result();
            if (paths.size() > 0 && _this->listNames.size() > 0) {
                try {
                    _this->importBookmarks(paths[0]);                      
                }
                catch(const std::exception& e)
                {
                    flog::error("{0}", e.what());
                    _this->txt_error = e.what();
                    _this->_error =true;                    
                }

            }
            delete _this->importDialog;
        }

        if (ImGui::GenericDialog(("manager_confirm4" + _this->name).c_str(), _this->_error, GENERIC_DIALOG_BUTTONS_OK, [_this]() {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Помилка імпорту json!  %s", _this->txt_error.c_str());
        }) == GENERIC_DIALOG_BUTTON_OK) {
            _this->importOpen = false;            
            _this->_error = false; 
        };

        if (_this->exportOpen && _this->exportDialog->ready()) {
            _this->exportOpen = false;
            std::string path = _this->exportDialog->result();
            if (path != "") {
                _this->exportBookmarks(path);
            }
            delete _this->exportDialog;
        }
    }
// -----------
    void start() {
        if (running) { return; }
        int _air_recording;
        core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);
        flog::info("AIR Recording is '{0}'", _air_recording);
        if(_air_recording==0)  {
            return;
        }        
        flog::info("void start(), running={0}", running);
        running = true;
        _recording = false;
        if(_detected==false)
             itbook = bookmarks.begin();  
       _detected = false;

        core::modComManager.callInterface("Запис", MAIN_SET_START, NULL, NULL);

        std::string folderPath = "%ROOT%/recordings";
        expandedLogPath = expandString(folderPath + genLogFileName("/freqmgr_"));

        curl = NULL;
        
        try {
            curl = curl_easy_init();  
        }catch (...) {

        }

        workerThread = std::thread(&ObservationManagerModule::worker, this);
    }

    void stop() {
        flog::info("void stop(), running={0}", running);
        running = false;

        if(_recording == true) {    
            flog::warn("STOP Receiving!");
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_STOP, NULL, NULL);
        }
        _recording = false;

        core::modComManager.callInterface("Запис", MAIN_SET_STOP, NULL, NULL);

//        if (!running) { return; }    
        usleep(1000);    
        running = false;
        _recording = false;

        if (workerThread.joinable()) {
            workerThread.join();
        }
        if(curr_nameWavFile!="")                            
            curlPOST_end(curr_nameWavFile);

        if(curl)
            curl_easy_cleanup(curl);        
    }

    void worker() {
        auto zirotime = std::chrono::time_point<std::chrono::high_resolution_clock>(std::chrono::seconds(1));
        firstSignalTime = zirotime;    
        int _count_rcv = 0;
        // 10Hz scan loop
        _lingerTime = _recordTime;
        // = _lingerTime;
        _Receiving = false;
        tuning = false;
        _detected = false;

        name = itbook->first;
        auto bm =   itbook->second;         
        bm.selected = true;
        applyMode(bm, gui::waterfall.selectedVFO);                     

        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            {
                std::lock_guard<std::mutex> lck(scan3Mtx);
                auto now = std::chrono::high_resolution_clock::now();

                // Enforce tuning
                if (gui::waterfall.selectedVFO.empty()) {
                    running = false;
                    return;
                }
                tuner::normalTuning(gui::waterfall.selectedVFO, current);            
                if (tuning && _Receiving==false) {
                    flog::warn("Tuning");
                    if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTuneTime)).count() > tuningTime) {
                        tuning = false;
                        _Receiving=true;
                        _count_rcv =0;
                        firstSignalTime = zirotime;
                    } else {
                        continue;
                    }

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

                // Check if we are waiting for a tune                    
                if (_Receiving) {
                    float maxLevel = getMaxLevel(data, current, vfoWidth, dataWidth, wfStart, wfWidth);
                    if (maxLevel >= (curr_level+3)) {
                        _count_rcv=3;                        
                        // flog::info("TRACE. Receiving... curr_level = {0}, maxLevel = {1}, current = {2}, _lingerTime {3}, _recording={4}, _record={5} !", curr_level, maxLevel, current, _lingerTime, _recording, _record);                        
                        if(firstSignalTime==zirotime) {
                            firstSignalTime = now;
                            int _mode;
                            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &_mode);
                            curr_nameWavFile = genWavFileName(current, _mode);
                            if(curr_nameWavFile!="")
                                 curlPOST_begin(curr_nameWavFile);

                            if(_record==true && running==true && _recording==false){ 
                                flog::info("TRACE. START Receiving! curr_level = {0}, maxLevel = {1}, current = {2} !", curr_level, maxLevel, current);
                                int recMode = 1;// RECORDER_MODE_AUDIO;
                                if(_mode==7) // TAW
                                    recMode = 0; //RECORDER_MODE_BASEBAND
                                core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                                core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_START, (void *) curr_nameWavFile.c_str(), NULL);
                                _recording = true;                            
                                
                            } else {
                                flog::info("TRACE. curr_level = {0}, maxLevel = {1}, current = {2} !", curr_level, maxLevel, current);
                            }  
                            std::string mylog = getNow() + ". Freq=" + std::to_string(current) + ", level=" + std::to_string(maxLevel) + "\r\n";                     
                            logfile = std::ofstream(expandedLogPath.c_str(), std::ios::binary | std::ios::app);
                            if(logfile.is_open()){
                                // flog::info("Recording to '{0}'", expandedPath);
                                logfile.write((char*)mylog.c_str(), mylog.size());
                                logfile.close();
                            }
                            else {
                                flog::error("Could not create '{0}'", expandedLogPath);
                            }
                            flog::info("TRACE. START Receiving... curr_level = {0}, maxLevel = {1}, current = {2}, _lingerTime {3}, _recording={4}, _record={5} !", curr_level, maxLevel, current, _lingerTime, _recording, _record);                        



                        }
                        lastSignalTime = now;
                        _detected = true; 
                        if (status_stop) {
                            if(_recording==true) {
                                if((std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSignalTime)).count() > _waitingTime) {
                                    core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_STOP, NULL, NULL);   
                                    _recording=false;   
                                }   
                            }
                        }  else {
                            if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - firstSignalTime)).count() > _lingerTime) { // = _recordTime
                                flog::info("TRACE. STOP Receiving! Next... curr_level = {0}, maxLevel = {1}, current = {2} !", curr_level, maxLevel, current);                           
                                if(_recording==true) 
                                    core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_STOP, NULL, NULL);   
                                _recording=false;   
                                _detected = false;                            
                                _Receiving = false;                          
                            }
                        }                     
                       
                    }
                    else {
                        flog::info("TRACE. Waiting... curr_level = {0}, maxLevel = {1}, current = {2}, _lingerTime {3}, _recording={4}, _count_rcv={5}, _detected {6} !", curr_level, maxLevel, current, _lingerTime, _recording, _count_rcv, _detected);
                        if(_count_rcv>2) {
                            if(_detected==true) {
                                if (status_stop) {
                                    if((std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSignalTime)).count() > _waitingTime) {
                                        if(_recording==true) 
                                            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_STOP, NULL, NULL);   
                                        _recording = false;
                                        _Receiving = false;
                                        _detected = false;                                         
                                    }   
                                } 
                            } else {
                                _Receiving = false;
                            }
                        } else 
                            _count_rcv++;
                    }
                    if(_Receiving==false) {    
                        if(curr_nameWavFile!="") {
                            curlPOST_end(curr_nameWavFile);
                            curr_nameWavFile ="";
                        }                      
                    }  
                }    
              
                if(_Receiving==false) {    
//                    flog::warn("Seeking signal");
//                    bm.selected = false;
                    itbook = next(itbook);
                    if(itbook==bookmarks.end()){ 
                        itbook=bookmarks.begin();
                    }    
                    name = itbook->first;
                    bm =   itbook->second;
                    current = bm.frequency;    
                    if(flag_level==true)
                        curr_level = level;
                    else                    
                        curr_level = bm.level;

                    flog::info("Set freq! bm[frequency] = {0}, bm[bandwidth] = {1}, bm[mode] = {2}, _name = {3}", bm.frequency, bm.bandwidth, bm.mode, name);
                    applyMode(bm, gui::waterfall.selectedVFO);   
                    if (gui::waterfall.selectedVFO.empty()) {
                        running = false;
                        return;
                    }

                    tuning = true;
                    lastTuneTime = now;
                    _detected = false;
                }

                // Release FFT Data
                gui::waterfall.releaseLatestFFT();
                /*
                if(status_stop==true && _detected==true){ 
                    running = false;                    
                }
                */

            }
        }
        // flog::info("record = {0}, bm[bandwidth] = {1}, bm[mode] = {2}, _name = {3}", bm.frequency, bm.bandwidth, bm.mode, name);        
    }

    bool findSignal(bool scanDir, double& bottomLimit, double& topLimit, double wfStart, double wfEnd, double wfWidth, double vfoWidth, float* data, int dataWidth) {
        bool found = false;
        double freq = current;           
            // Check signal level
        if(_recording==false) {                
            float maxLevel = getMaxLevel(data, freq, vfoWidth * (passbandRatio * 0.01f), dataWidth, wfStart, wfWidth);
            if (maxLevel >= level) {
                found = true;
                _Receiving = true;
                current = freq;
            }
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
        if(max<-150) max=-150;
        return max;
    }
    
    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        ObservationManagerModule* _this = (ObservationManagerModule*)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }

        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP) {
            for (auto const bm : _this->waterfallBookmarks) {
                double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
// flog::error("TOP bm.bookmarkName.c_str() ={0}", bm.bookmarkName.c_str());

                if (bm.bookmark.frequency >= args.lowFreq && bm.bookmark.frequency <= args.highFreq) {
                    args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), IM_COL32(255, 255, 0, 255));
                }

                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.min.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.min.y + nameSize.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

                if (clampedRectMax.x - clampedRectMin.x > 0) {
                    args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, IM_COL32(255, 255, 0, 255));
                }
                if (rectMin.x >= args.min.x && rectMax.x <= args.max.x) {
                    args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), args.min.y), IM_COL32(0, 0, 0, 255), bm.bookmarkName.c_str());
                }
            }
        }
        else if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) {
            for (auto const bm : _this->waterfallBookmarks) {
                double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);

                if (bm.bookmark.frequency >= args.lowFreq && bm.bookmark.frequency <= args.highFreq) {
                    args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), IM_COL32(255, 255, 0, 255));
                }
// flog::error("BOTTOM bm.bookmarkName.c_str() ={0}", bm.bookmarkName.c_str());
                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.max.y - nameSize.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.max.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

                if (clampedRectMax.x - clampedRectMin.x > 0) {
                    args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, IM_COL32(255, 255, 0, 255));
                }
                if (rectMin.x >= args.min.x && rectMax.x <= args.max.x) {
                    args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), args.max.y - nameSize.y), IM_COL32(0, 0, 0, 255), bm.bookmarkName.c_str());
                }
            }
        }
    }

    bool mouseAlreadyDown = false;
    bool mouseClickedInLabel = false;
    static void fftInput(ImGui::WaterFall::InputHandlerArgs args, void* ctx) {
        ObservationManagerModule* _this = (ObservationManagerModule*)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }

        if (_this->mouseClickedInLabel) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                _this->mouseClickedInLabel = false;
            }
            gui::waterfall.inputHandled = true;
            return;
        }
        return;
        // First check that the mouse clicked outside of any label. Also get the bookmark that's hovered
        bool inALabel = false;
        WaterfallBookmark hoveredBookmark;
        std::string hoveredBookmarkName;

        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP) {
            int count = _this->waterfallBookmarks.size();
            for (int i = count - 1; i >= 0; i--) {
                auto& bm = _this->waterfallBookmarks[i];
                double centerXpos = args.fftRectMin.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.fftRectMin.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.fftRectMin.y + nameSize.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.fftRectMin.x, args.fftRectMax.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.fftRectMin.x, args.fftRectMax.x), rectMax.y);

                if (ImGui::IsMouseHoveringRect(clampedRectMin, clampedRectMax)) {
                    inALabel = true;
                    hoveredBookmark = bm;
                    hoveredBookmarkName = bm.bookmarkName;
                    break;
                }
            }
        }
        else if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) {
            int count = _this->waterfallBookmarks.size();
            for (int i = count - 1; i >= 0; i--) {
                auto& bm = _this->waterfallBookmarks[i];
                double centerXpos = args.fftRectMin.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.fftRectMax.y - nameSize.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.fftRectMax.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.fftRectMin.x, args.fftRectMax.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.fftRectMin.x, args.fftRectMax.x), rectMax.y);
                if (ImGui::IsMouseHoveringRect(clampedRectMin, clampedRectMax)) {
                    inALabel = true;
                    hoveredBookmark = bm;
                    hoveredBookmarkName = bm.bookmarkName;
                    break;
                }
            }
        }

        // Check if mouse was already down
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !inALabel) {
            _this->mouseAlreadyDown = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            _this->mouseAlreadyDown = false;
            _this->mouseClickedInLabel = false;
        }

        // If yes, cancel
        if (_this->mouseAlreadyDown || !inALabel) { return; }

        gui::waterfall.inputHandled = true;

        double centerXpos = args.fftRectMin.x + std::round((hoveredBookmark.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
        ImVec2 nameSize = ImGui::CalcTextSize(hoveredBookmarkName.c_str());
        ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) ? (args.fftRectMax.y - nameSize.y) : args.fftRectMin.y);
        ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) ? args.fftRectMax.y : args.fftRectMin.y + nameSize.y);
        ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.fftRectMin.x, args.fftRectMax.x), rectMin.y);
        ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.fftRectMin.x, args.fftRectMax.x), rectMax.y);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            _this->mouseClickedInLabel = true;
            applyBookmark(hoveredBookmark.bookmark, gui::waterfall.selectedVFO);
        }

        ImGui::BeginTooltip();
        ImGui::TextUnformatted(hoveredBookmarkName.c_str());
        ImGui::Separator();
        ImGui::Text("Банк: %s", hoveredBookmark.listName.c_str());
        ImGui::Text("Частота: %s", utils::formatFreq(hoveredBookmark.bookmark.frequency).c_str());
        ImGui::Text("Ширина Смуги: %s", utils::formatFreq(hoveredBookmark.bookmark.bandwidth).c_str());
        ImGui::Text("Поріг виявлення: %s", utils::formatFreq(hoveredBookmark.bookmark.level).c_str());
        ImGui::Text("Режим: %s", demodModeList[hoveredBookmark.bookmark.mode]);

        ImGui::EndTooltip();
    }

    json exportedBookmarks;
    bool importOpen = false;
    bool exportOpen = false;
    pfd::open_file* importDialog;
    pfd::save_file* exportDialog;

    void importBookmarks(std::string path) {
        std::ifstream fs(path);
        json importBookmarks;
        fs >> importBookmarks;

        if (!importBookmarks.contains("bookmarks")) {
            flog::error("File does not contains any bookmarks");
            return;
        }

        if (!importBookmarks["bookmarks"].is_object()) {
            flog::error("Bookmark attribute is invalid");
            return;
        }

        // Load every bookmark
        for (auto const [_name, bm] : importBookmarks["bookmarks"].items()) {
                if (bookmarks.find(_name) != bookmarks.end()) {
                flog::warn("Bookmark with the name '{0}' already exists in list, skipping", _name);
                continue;
            }
            ObservationBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.mode = bm["mode"];
            fbm.level = bm["level"];
            fbm.selected = false;
            bookmarks[_name] = fbm;
        }
        saveByName(selectedListName);

        fs.close();
    }

    void exportBookmarks(std::string path) {
        std::ofstream fs(path);
        exportedBookmarks >> fs;
        fs.close();
    }

    std::string expandString(std::string input) {
        input = std::regex_replace(input, std::regex("%ROOT%"), root);
        return std::regex_replace(input, std::regex("//"), "/");
    }

    static void moduleInterfaceHandler(int freq, void* in, void* out, void* ctx) {
        ObservationManagerModule* _this = (ObservationManagerModule*)ctx;
        flog::info("moduleInterfaceHandler, name = {0}",  _this->selectedListName);
        
        struct FreqData {
            int freq;
            int mode;
            float level;
        } pFreqData;
        pFreqData =  *(static_cast<FreqData *>(in)); 
                // pFreqData.freq = _this->current;            
            int _mode = pFreqData.mode; //  *(int*)in;
            if (gui::waterfall.selectedVFO == "") {
                _this->editedBookmark.frequency = pFreqData.freq; 
                _this->editedBookmark.bandwidth = 0;
                _this->editedBookmark.mode = (int) _mode;
            }
            else {
                _this->editedBookmark.frequency = pFreqData.freq;
                _this->editedBookmark.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
                _this->editedBookmark.mode = (int) _mode;
                /*
                if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio") {
                    int mode=1;
                 //   core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                    _this->editedBookmark.mode = mode;
                }
                */
            }

            _this->editedBookmark.level = pFreqData.level; // _this->level;

            _this->editedBookmark.selected = false;

//            _this->createOpen = true;

            // Find new unique default name
            if (_this->bookmarks.find("1") == _this->bookmarks.end()) {
                _this->editedBookmarkName = "1";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    sprintf(buf, "%d", i);
                    if (_this->bookmarks.find(buf) == _this->bookmarks.end()) { break; }
                }
                _this->editedBookmarkName = buf;
            }

                // If editing, delete the original one
 //              if (editOpen) {
                _this->bookmarks.erase(_this->firstEditedBookmarkName);
//                }
                _this->bookmarks[_this->editedBookmarkName] = _this->editedBookmark;

                _this->saveByName(_this->selectedListName);

                 
    }

//=====================================================    
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
                            if(curl) {
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

    bool curlPOST_end(std::string fname) {
                            if(curl) {
                                // const char *url = "http://localhost:18101/event/end";
                                std::string url = thisURL + "/begin";
                                char curlErrorBuffer[CURL_ERROR_SIZE];
                                curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuffer);
                                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                                curl_easy_setopt(curl, CURLOPT_POST, 1);
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
//=====================================================    


    std::string name;
    bool enabled = true;
    bool createOpen = false;
    bool editOpen = false;
    bool newListOpen = false;
    bool renameListOpen = false;
    bool selectListsOpen = false;

    bool deleteListOpen = false;
    bool deleteBookmarksOpen = false;

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
    EventHandler<ImGui::WaterFall::InputHandlerArgs> inputHandler;

    std::map<std::string, ObservationBookmark> bookmarks;
    std::map<std::string, ObservationBookmark>::iterator itbook;

    std::string editedBookmarkName = "";
    std::string firstEditedBookmarkName = "";
    ObservationBookmark editedBookmark;

    std::vector<std::string> listNames;
    std::string listNamesTxt = "";
    std::string selectedListName = "";
    int selectedListId = 0;
    bool status_stop = false;

    std::string editedListName;
    std::string firstEditedListName;

    std::vector<WaterfallBookmark> waterfallBookmarks;

    int bookmarkDisplayMode = 0;


    bool running = false;
    std::thread workerThread;
    std::mutex scan3Mtx;
 
//    double startFreq = 80000000.0;
//    double stopFreq = 160000000.0;
//    double interval = 10000.0;

    double current = 88000000.0;
    double passbandRatio = 10.0;
    int tuningTime = 300;
    int _lingerTime = 4000.0;
    int _waitingTime = 1000;
    float level = -100.0;
    bool _Receiving = true;
    bool tuning = false;
    bool scanUp = true;
    bool reverseLock = false;
    bool _record = true;
    int  _recordTime = 3000;    
    bool _recording = false;
    bool _detected = false;
    std::ofstream logfile;
    std::string root = (std::string)core::args["root"];
    int curr_level = -50;
    bool flag_level = false;

    std::chrono::time_point<std::chrono::high_resolution_clock> lastSignalTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> lastTuneTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> firstSignalTime;

    std::string expandedLogPath;
    std::string curr_listName="";

    std::string curr_nameWavFile = "";

    std::vector<uint32_t> bandwidthsList;
    int _frec = 0, _bandwidthId = 0;
    bool _raw = false;

    std::string txt_error="";
    bool _error = false;

    std::string thisURL = "http://localhost:18101/event/";
    std::string thisInstance = "test";
    CURL *curl;

};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["selectedList"] = "General";
    def["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    def["lists"]["General"]["showOnWaterfall"] = true;
    def["lists"]["General"]["bookmarks"] = json::object();

    config.setPath(core::args["root"].s() + "/frequency_manager_config.json");
    config.load(def);
    config.enableAutoSave();

    // Check if of list and convert if they're the old type
    config.acquire();
    if (!config.conf.contains("bookmarkDisplayMode")) {
        config.conf["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    }
    for (auto [listName, list] : config.conf["lists"].items()) {
        if (list.contains("bookmarks") && list.contains("showOnWaterfall") && list["showOnWaterfall"].is_boolean()) { continue; }
        json newList;
        newList = json::object();
        newList["showOnWaterfall"] = true;
        newList["bookmarks"] = list;
        config.conf["lists"][listName] = newList;
    }
    config.release(true);
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ObservationManagerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (ObservationManagerModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
