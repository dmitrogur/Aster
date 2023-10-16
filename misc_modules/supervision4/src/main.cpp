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
//#include <portaudio.h>
// #include <RtAudio.h>
#include "/usr/include/rtaudio/RtAudio.h"

#include <dsp/convert/stereo_to_mono.h>
#include <dsp/sink/ring_buffer.h>
#include <utils/freq_formatting.h>
#include <fstream>

#include <core.h>
#include <ctime>
#include <chrono>

#include <curl/curl.h>

#define MAX_CHANNELS 10

SDRPP_MOD_INFO{
    /* Name:            */ "supervision4",
    /* Description:     */ "Multichannel observation manager module for Aster",
    /* Author:          */ "DMH",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ 1
};

enum {
    AIRSPY_IFACE_CMD_GET_RECORDING,
    AIRSPY_IFACE_CMD_START,
    AIRSPY_IFACE_CMD_STOP,
    AIRSPY_IFACE_NAME_SER_NOM
};

enum {
    AUDIO_SINK_GET_DEVLIST,
    AUDIO_SINK_CMD_SET_DEV
};


struct ObservationBookmark {
    double frequency;
    float bandwidth;
    int mode;
    int level;
    std::string scard;
    bool selected;
};

struct WaterfallBookmark {
    std::string listName;
    std::string bookmarkName;
    ObservationBookmark bookmark;
};

ConfigManager config;


enum {
    BOOKMARK_DISP_MODE_OFF,
    BOOKMARK_DISP_MODE_TOP,
    BOOKMARK_DISP_MODE_BOTTOM,
    _BOOKMARK_DISP_MODE_COUNT
};

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


class SupervisorModeModule : public ModuleManager::Instance {
public:
    SupervisorModeModule(std::string name) {
        this->name = name;

        config.acquire();
        std::string selList = config.conf["selectedList"];
        bookmarkDisplayMode = config.conf["bookmarkDisplayMode"];
        config.release();

        refreshLists();
        loadByName(selList);
        refreshWaterfallBookmarks();

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        inputHandler.ctx = this;
        inputHandler.handler = fftInput;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.bindHandler(&inputHandler);

        root = (std::string)core::args["root"];        
        flog::info("name={0}", name);

        for(int i=0;i<MAX_CHANNELS;i++)
            ch_recording[i] = false;

        thisURL      = core::configManager.conf["Url"];
        thisInstance = core::configManager.conf["InstanceName"];
        thisInstance = thisInstance + "-4";

        bandwidthsList.clear();
        bandwidthsList.push_back(1000);
        bandwidthsList.push_back(6250);
        bandwidthsList.push_back(12500);
        bandwidthsList.push_back(25000);
        bandwidthsList.push_back(50000);
        bandwidthsList.push_back(100000);
        bandwidthsList.push_back(220000);

        core::modComManager.registerInterface("supervision4", name, moduleInterfaceHandler, this);
    }

    ~SupervisorModeModule() {
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
                if (core::modComManager.getModuleName(vfoName) == "Канал приймання") {
                    int mode = bm.mode;
                    float bandwidth = bm.bandwidth;
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                }
            }            
         tuner::normalTuning(gui::waterfall.selectedVFO, bm.frequency);
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
                if (core::modComManager.getModuleName(vfoName) == "Канал приймання") {
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

        std::string id = "Edit##supervisor_edit_popup_4" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[1024];
        strcpy(nameBuf, editedBookmarkName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            ImGui::BeginTable(("supervisor_edit_table" + name).c_str(), 2);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Назва");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);
            if (ImGui::InputText(("##supervisor_edit_name_4" + name).c_str(), nameBuf, 1023)) {
                editedBookmarkName = nameBuf;
//               flog::info("!!! editedBookmarkName={0}, nameBuf={1}", editedBookmarkName, nameBuf);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Частота, Гц");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);
            // ImGui::InputDouble(("##supervisor_edit_freq_4" + name).c_str(), &editedBookmark.frequency);
            ImGui::InputInt(("##supervisor_edit_freq_4" + name).c_str(), &_frec, 100, 100000);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Смуга, Гц");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);
            // ImGui::InputDouble(("##supervisor_edit_bw_4" + name).c_str(), &editedBookmark.bandwidth);
            if (ImGui::Combo(("##fsupervisor_edit_bw_4" + name).c_str(), &_bandwidthId, bandListTxt)) {
                editedBookmark.bandwidth = bandwidthsList[_bandwidthId];
                flog::info("TRACE. editedBookmark.bandwidth = {0} !", editedBookmark.bandwidth);
            }            

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Режим");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);

            // ImGui::Combo(("##supervisor_edit_mode_4" + name).c_str(), &editedBookmark.mode, demodModeListTxt);
            if (ImGui::Combo(("##supervisor_edit_mode_4" + name).c_str(), &editedBookmark.mode, demodModeListTxt)) {
                editedBookmark.mode = editedBookmark.mode;
                if(editedBookmark.mode==7) {
                    _raw = true;
                    editedBookmark.bandwidth = 220000;
                    _bandwidthId = 6;
                } else {
                    _raw =false;
                }       

                flog::info("TRACE. editedBookmark.mode = {0} !", editedBookmark.mode);
            }

            
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Поріг, дБ");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);
            ImGui::InputInt(("##supervisor_edit_level_4" + name).c_str(), &editedBookmark.level, -150, 0);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Звукова карта");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(250);
            
            if (ImGui::Combo(("##supervisor_dev_4" + name).c_str(), &devListId, currDevList.c_str())) {    
                editedBookmark.scard = currVCardDevList[devListId];
                flog::info("!!! devListId={0}, currVCardDevList[devListId]={1}", devListId, currVCardDevList[devListId]);
             }
         
            ImGui::EndTable();


            bool applyDisabled = (strlen(nameBuf) == 0) || (bookmarks.find(editedBookmarkName) != bookmarks.end() && editedBookmarkName != firstEditedBookmarkName) || (currDevList=="");
            if(applyDisabled==false && editOpen==false)  {
                for (auto& [name, bm] : bookmarks) {
                    if(bm.frequency==editedBookmark.frequency) {
                        applyDisabled = true;
                        break;
                    }    
                }        
            }
     
            if (applyDisabled) { style::beginDisabled(); }
            if (ImGui::Button("OK")) { /// Apply
                open = false;
                editedBookmark.frequency = _frec;
                if(_raw==true) {
                    editedBookmark.bandwidth = sigpath::iqFrontEnd.getSampleRate();
                }

                if (!editOpen) {
                    for (auto& [name, bm] : bookmarks) {
                        if(name==editedBookmarkName) {
                            flog::error("!!! ERROR add new freq  {0}", editedBookmarkName);
                            return false;
                        }    
                    }              
                }                 

                flog::info("!!! 1  scard={0}, devListId={1}, editedBookmark.scard={1}, firstEditedBookmarkName = {2}", editedBookmark.scard,  devListId, currVCardDevList[devListId]);
                if(editedBookmark.scard=="") {

                    devListId = 0;
                    flog::info("!!! editedBookmark.scard=0! scard={0}, devListId={1}, editedBookmark.scard={1}, firstEditedBookmarkName = {2}", editedBookmark.scard,  devListId, currVCardDevList[devListId]);
 
                    if(!currVCardDevList.empty()) {
                        editedBookmark.scard=currVCardDevList[0];
                        flog::info("!!! Ok. editedBookmark.scard {0}", editedBookmark.scard);
                    } else  {
                        flog::error("!!! ERROR Not free sound card! currVCardDevList is empty()");
                        return false;
                    }
                }
                flog::info("!!! 2 editedBookmarkName={0}, editedBookmark.scard={1}, firstEditedBookmarkName = {2}, editOpen={3}", editedBookmarkName, editedBookmark.scard, firstEditedBookmarkName, editOpen);

                if (editOpen) {
                    bool _del = false;
                    core::moduleManager.deleteInstance(firstEditedBookmarkName);
                    std::string rec_name = "Запис " + firstEditedBookmarkName;
                    core::moduleManager.deleteInstance(rec_name);
                    _del = true;
                    if(_del==true)
                        bookmarks.erase(firstEditedBookmarkName);                      
                    else {
                        flog::error("!!! ERROR delete Instance {0}", firstEditedBookmarkName);
                        return false;
                    }    
                }

                ObservationBookmark& bm = editedBookmark;
                double _frequency = editedBookmark.frequency ; 
                double _offset =  sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
                double _central =gui::waterfall.getCenterFrequency();

                std::string str = editedBookmarkName;          
                char modName[1024];                      
                char recmodName[1024];                      

                strcpy(modName, (char *) str.c_str());
                bool modified= false; 
                flog::info("!!! modName={0}, editedBookmark.frequency={1}", modName, str.c_str());
// Add Radio
                if (!core::moduleManager.createInstance(modName, "radio")) {
                    core::moduleManager.postInit(modName);
                    modified = true;
                    flog::info("!!! Канал приймання successfully added! modName={0}, editedBookmarkName={1} editedBookmark.frequency={2}", modName,  editedBookmarkName, editedBookmark.frequency);
                } else {
                    flog::error("!!! ERROR adding radio. modName={0}, editedBookmarkName={1}, editedBookmark.frequency={2}", modName, editedBookmarkName, editedBookmark.frequency);
                }

// Add Radio
                std::string recName = "Запис " + editedBookmarkName;
                strcpy(recmodName, (char *) recName.c_str());
                if (!core::moduleManager.createInstance(recmodName, "recorder")) {
                    core::moduleManager.postInit(recmodName);
                    modified = true;
                    flog::info("!!! Recorder successfully added! modName={0}, editedBookmarkName={1} editedBookmark.frequency={2}", recmodName,  editedBookmarkName, editedBookmark.frequency);
                } else {
                    if(modified==true) {
                        core::moduleManager.deleteInstance(modName);
                    }
                    modified = false;
                    flog::error("!!! ERROR adding Recorder! modName={0}, editedBookmarkName={1}, editedBookmark.frequency={2}", recmodName, editedBookmarkName, editedBookmark.frequency);
                }


                flog::info(" TRACE centerFrequency = {0}, editedBookmark.frequency = {1}, gui::waterfall.selectedVFO = {2} ", gui::waterfall.getCenterFrequency(), _frequency, gui::waterfall.selectedVFO);                    

                if (modified) {
                    // If editing, delete the original one
                    // recName
                    struct setDev{
                        std::string sndname; 
                        int id;
                    } psetDev;
                    psetDev.id = devListId;
                    psetDev.sndname = modName;

                    core::modComManager.callInterface("Аудіо вивід", AUDIO_SINK_CMD_SET_DEV, &psetDev, NULL);            

                    if (editOpen) {
                        bookmarks.erase(firstEditedBookmarkName);
                    }
                    bookmarks[editedBookmarkName] = editedBookmark;

                    saveByName(selectedListName);

                    // Update enabled and disabled modules
                    core::configManager.acquire();
                    json instances;
                    for (auto [_name, inst] : core::moduleManager.instances) {
                        instances[_name]["module"] = inst.module.info->name;
                        instances[_name]["enabled"] = inst.instance->isEnabled();
                        // flog::warn("TRACE! _name =  {0}", _name);
                    }
                    core::configManager.conf["moduleInstances"] = instances;
                    core::configManager.release(true);                                      
                    gui::waterfall.setCurrVFO(editedBookmarkName);
                    gui::waterfall.selectedVFO = editedBookmarkName;

                    usleep(200);

                    tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, bm.frequency);

                    applyMode(bm, gui::waterfall.selectedVFO);

                    int mode = bm.mode;
                    float bandwidth = bm.bandwidth;
                    core::modComManager.callInterface(editedBookmarkName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(editedBookmarkName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);

                    int stream = 1;
                    for (auto& [name, bm] : bookmarks) {
                        if(name==editedBookmarkName)
                            break;
                        stream++;
                    }      

                    gui::waterfall.setCenterFrequency(_central);
                    sigpath::vfoManager.setOffset(gui::waterfall.selectedVFO, _offset);
//                    usleep(100);        
                    gui::waterfall.centerFreqMoved = true;

                    flog::info(" 4 TRACE gui::waterfall.selectedVFO = {0}, stream= {1}, recName={2}", gui::waterfall.selectedVFO, stream, recName);                    

                    core::modComManager.callInterface(recName, RECORDER_IFACE_CMD_SET_STREAM, &stream, NULL);

                    flog::info(" 5 TRACE gui::waterfall.selectedVFO = {0}, stream= {1}", gui::waterfall.selectedVFO, stream);                    

                    flog::info(" TRACE centerFrequency = {0}, editedBookmark.frequency = {1}, _offset = {2}, gui::waterfall.selectedVFO = {3}, stream ={4} ", gui::waterfall.getCenterFrequency(), _frequency, _offset, gui::waterfall.selectedVFO, stream);                    
                    gui::waterfall.setCurrVFO("Канал приймання");
                    usleep(200);        


                    flog::info(" TRACE gui::waterfall.selectedVFO = {0}", gui::waterfall.selectedVFO);                    
                }

//                      tuner::normalTuning(gui::waterfall.selectedVFO, current);
            
            }
            if (applyDisabled) { style::endDisabled(); }
            ImGui::SameLine();
            if (ImGui::Button("Скасувати")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        bookmarks_size = bookmarks.size();
        return open;
    }

    bool newListDialog() {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "New##supervisor_new_popup_4" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[1024];
        strcpy(nameBuf, editedListName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            ImGui::LeftLabel("Назва банку ");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputText(("##supervisor_edit_name_4" + name).c_str(), nameBuf, 1023)) {
                editedListName = nameBuf;
            }

            bool alreadyExists = (std::find(listNames.begin(), listNames.end(), editedListName) != listNames.end());

            if (strlen(nameBuf) == 0 || alreadyExists) { style::beginDisabled(); }
            if (ImGui::Button("OK")) {
                open = false;

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

        std::string id = "Вибір Банку##supervisor_sel_popup_4" + name;
        ImGui::OpenPopup(id.c_str());

        bool open = true;

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            // No need to lock config since we're not modifying anything and there's only one instance
            for (auto [listName, list] : config.conf["lists"].items()) {
                bool shown = list["showOnWaterfall"];
                if (ImGui::Checkbox((listName + "##supervisor_sel_list_4").c_str(), &shown)) {
                    config.acquire();
                    config.conf["lists"][listName]["showOnWaterfall"] = shown;
                    refreshWaterfallBookmarks(false);
                    config.release(true);
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
            if(listName!=selectedListName)
                        continue;
flog::info("showOnWaterfall ={0}", listName.c_str());                        

            WaterfallBookmark wbm;
            wbm.listName = listName;
            for (auto [bookmarkName, bm] : config.conf["lists"][listName]["bookmarks"].items()) {
                wbm.bookmarkName = bookmarkName;
                wbm.bookmark.frequency = config.conf["lists"][listName]["bookmarks"][bookmarkName]["frequency"];
                wbm.bookmark.bandwidth = config.conf["lists"][listName]["bookmarks"][bookmarkName]["bandwidth"];
                wbm.bookmark.mode      = config.conf["lists"][listName]["bookmarks"][bookmarkName]["mode"];
                try
                {
                    wbm.bookmark.scard      = config.conf["lists"][listName]["bookmarks"][bookmarkName]["scard"];
                }
                catch(const std::exception& e)
                {
                    std::cerr << e.what() << '\n';
                    wbm.bookmark.scard      = "";
                }
                try
                {
                    wbm.bookmark.level = config.conf["lists"][listName]["bookmarks"][bookmarkName]["level"];
                } 
                catch(const std::exception& e)
                {
                    std::cerr << e.what() << '\n';
                    wbm.bookmark.level      = -70;
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
        config.acquire();
        for (auto [bmName, bm] : config.conf["lists"][listName]["bookmarks"].items()) {
            ObservationBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.mode = bm["mode"];
            
            try
            {
                fbm.scard = bm["scard"];
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << '\n';
                fbm.scard = "";
            }
            try
            {
                fbm.level = bm["level"];
            } 
            catch(const std::exception& e)
            {
                std::cerr << e.what() << '\n';
                fbm.level = -70;
            }           
            fbm.selected = false;
            bookmarks[bmName] = fbm;
        }
        config.release();
        bookmarks_size = bookmarks.size();
    }

void loadListToInstance(std::string listName) {
        bookmarks.clear();
        if (std::find(listNames.begin(), listNames.end(), listName) == listNames.end()) {
            selectedListName = "";
            selectedListId = 0;
            loadFirst();
            return;
        }
        selectedListId = std::distance(listNames.begin(), std::find(listNames.begin(), listNames.end(), listName));
        selectedListName = listName;
        config.acquire();
        for (auto [bmName, bm] : config.conf["lists"][listName]["bookmarks"].items()) {
            ObservationBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.mode = bm["mode"];
            
            try
            {
                fbm.scard = bm["scard"];
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << '\n';
                fbm.scard = "";
            }
            try
            {
                fbm.level = bm["level"];
            } 
            catch(const std::exception& e)
            {
                std::cerr << e.what() << '\n';
                fbm.level = -70;
            }           
            fbm.selected = false;
            bookmarks[bmName] = fbm;
        }
        config.release();
        bookmarks_size = bookmarks.size();
    }

    void saveByName(std::string listName) {
        config.acquire();
        config.conf["lists"][listName]["bookmarks"] = json::object();
        for (auto [bmName, bm] : bookmarks) {
            config.conf["lists"][listName]["bookmarks"][bmName]["frequency"] = bm.frequency;
            config.conf["lists"][listName]["bookmarks"][bmName]["bandwidth"] = bm.bandwidth;
            config.conf["lists"][listName]["bookmarks"][bmName]["mode"]      = bm.mode;
            config.conf["lists"][listName]["bookmarks"][bmName]["level"]     = bm.level;
            config.conf["lists"][listName]["bookmarks"][bmName]["scard"]     = bm.scard;
        }
        refreshWaterfallBookmarks(false);
        config.release(true);
    }

    bool AddSelectedList(){    
                for (auto [bmName, bm] : config.conf["lists"][selectedListName]["bookmarks"].items()) {
                    ObservationBookmark fbm;
                    std::string _name = bmName;
                    flog::warn("TRACE! Add Instance {0}", _name);
                    if (!core::moduleManager.createInstance(_name, "radio")) {
                        core::moduleManager.postInit(_name);
                        flog::info("!!! Канал приймання successfully added! modName={0}, editedBookmarkName={1} editedBookmark.frequency={2}", _name,  editedBookmarkName, editedBookmark.frequency);
                    } else {
                        flog::error("!!! ERROR adding radio. modName={0}, editedBookmarkName={1}, editedBookmark.frequency={2}", _name, editedBookmarkName, editedBookmark.frequency);
                    }

                    _name = "Запис " + _name;
                    flog::warn("TRACE! Add Instance {0}", _name);
                    if (!core::moduleManager.createInstance(_name, "recorder")) {
                        core::moduleManager.postInit(_name);
                        flog::info("!!! Recorder successfully added! modName={0}, editedBookmarkName={1} editedBookmark.frequency={2}", _name,  editedBookmarkName, editedBookmark.frequency);
                    } else {
                        flog::error("!!! ERROR adding Recorder! modName={0}, editedBookmarkName={1}, editedBookmark.frequency={2}", _name, editedBookmarkName, editedBookmark.frequency);
                    }
                }  

                    // Update enabled and disabled modules
                core::configManager.acquire();
                json instances;
                for (auto [_name, inst] : core::moduleManager.instances) {
                    instances[_name]["module"] = inst.module.info->name;
                    instances[_name]["enabled"] = inst.instance->isEnabled();
                    // flog::warn("TRACE! _name =  {0}", _name);
                }
               core::configManager.conf["moduleInstances"] = instances;
               core::configManager.release(true); 

        return true;
    }

    bool DelSelectedList(){
                for (auto [bmName, bm] : config.conf["lists"][selectedListName]["bookmarks"].items()) {
                    ObservationBookmark fbm;
                    std::string _name = bmName;
                    flog::warn("TRACE! _this->selectedListName) {0}", _name);
                    core::moduleManager.deleteInstance(_name);
                    std::string rec_name = "Запис " + _name;
                    core::moduleManager.deleteInstance(rec_name);
      
                    // _this->bookmarks.erase(_name);   
                }    
                    // Update enabled and disabled modules
                core::configManager.acquire();
                json instances;
                for (auto [_name, inst] : core::moduleManager.instances) {
                    instances[_name]["module"] = inst.module.info->name;
                    instances[_name]["enabled"] = inst.instance->isEnabled();
                    // flog::warn("TRACE! _name =  {0}", _name);
                }
               core::configManager.conf["moduleInstances"] = instances;
               core::configManager.release(true); 
        return true;
    }

    bool DelSelectedFreq(std::string _name){
        sigpath::sinkManager.stopStream(_name);
//        sigpath::sinkManager.unregisterStream(_name);
        flog::warn("TRACE! _this->selectedListName) {0}", _name);
        core::moduleManager.deleteInstance(_name);

        std::string rec_name = "Запис " + _name;
        core::moduleManager.deleteInstance(rec_name);

        // Update enabled and disabled modules
        core::configManager.acquire();
        json instances;
        for (auto [_name, inst] : core::moduleManager.instances) {
            instances[_name]["module"] = inst.module.info->name;
            instances[_name]["enabled"] = inst.instance->isEnabled();

        }
        core::configManager.conf["moduleInstances"] = instances;


        core::configManager.release(true); 


        return true;
    }

    static void menuHandler(void* ctx) {
        SupervisorModeModule* _this = (SupervisorModeModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        int _work;
        core::modComManager.callInterface("Запис", MAIN_GET_PROCESSING, NULL, &_work);

        if (_this->running || gui::waterfall.selectedVFO!="Канал приймання" || _work>0) { ImGui::BeginDisabled();  }

        // TODO: Replace with something that won't iterate every frame
        std::vector<std::string> selectedNames;
        for (auto& [name, bm] : _this->bookmarks) {
            if (bm.selected) { selectedNames.push_back(name); }
        }

        float lineHeight = ImGui::GetTextLineHeightWithSpacing();

        float btnSize = ImGui::CalcTextSize("Зберегти").x + 8;
        ImGui::SetNextItemWidth(menuWidth - 24 - (2 * lineHeight) - btnSize);
        if (ImGui::Combo(("##supervisor_list_sel_4" + _this->name).c_str(), &_this->selectedListId, _this->listNamesTxt.c_str())) {
            if(_this->listNames[_this->selectedListId]!=_this->selectedListName) {
                _this->DelSelectedList();
                config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = false;
                _this->loadByName(_this->listNames[_this->selectedListId]);
                config.acquire();
                config.conf["selectedList"] = _this->selectedListName;
                config.release(true);

                _this->AddSelectedList();

                config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = true;

                _this->refreshWaterfallBookmarks(false);

            }
        }
        ImGui::SameLine();
        if (_this->listNames.size() == 0) { style::beginDisabled(); }
        if (ImGui::Button(("Зберегти##supervisor_ren_lst_4" + _this->name).c_str(), ImVec2(btnSize, 0))) {
            _this->firstEditedListName = _this->listNames[_this->selectedListId];
            _this->editedListName = _this->firstEditedListName;
            _this->renameListOpen = true;
        }
        if (_this->listNames.size() == 0) { style::endDisabled(); }
        ImGui::SameLine();
        if (ImGui::Button(("+##supervisor_add_lst_4" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
                _this->DelSelectedList();
                config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = false;
                _this->refreshWaterfallBookmarks(true);

            // Find new unique default name
            if (std::find(_this->listNames.begin(), _this->listNames.end(), "Новий банк") == _this->listNames.end()) {
                _this->editedListName = "Новий банк";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    sprintf(buf, "Новий банк (%d)", i);
                    if (std::find(_this->listNames.begin(), _this->listNames.end(), buf) == _this->listNames.end()) { break; }
                }
                _this->editedListName = buf;    bool _record = true;

            }
            _this->newListOpen = true;
        }
        ImGui::SameLine();
        if (_this->selectedListName == "") { style::beginDisabled(); }
        if (ImGui::Button(("-##supervisor__del_lst_4" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
            _this->deleteListOpen = true;
        }
        if (_this->selectedListName == "") { style::endDisabled(); }

        // List delete confirmation
        if (ImGui::GenericDialog(("supervisor_del_list_confirm_4" + _this->name).c_str(), _this->deleteListOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::Text("Видалення банку \"%s\". Ви впевнені?", _this->selectedListName.c_str());
            }) == GENERIC_DIALOG_BUTTON_YES) {
            if(_this->selectedListName!="General"){    
                _this->DelSelectedList();
                 
                // _this->saveByName(_this->selectedListName);                    

                config.acquire();
                config.conf["lists"].erase(_this->selectedListName);
                config.release(true);
                _this->refreshLists();
                _this->selectedListId = std::clamp<int>(_this->selectedListId, 0, _this->listNames.size());
                if (_this->listNames.size() > 0) {
                    _this->loadByName(_this->listNames[_this->selectedListId]);
                }
                else {
                    _this->selectedListName = "";
                }     
                _this->AddSelectedList();
                config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = true;
                _this->refreshWaterfallBookmarks(false);
            }
        }

        if (_this->selectedListName == "") { style::beginDisabled(); }

       //Draw import and export buttons
       
        ImGui::BeginTable(("supervisor_bottom_btn_table_4" + _this->name).c_str(), 2);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Iмпорт##supervisor_imp_4" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->importOpen) {
            _this->importOpen = true;

            try {
                _this->importDialog = new pfd::open_file("Import bookmarks", "", { "JSON Files (*.json)", "*.json", "All Files", "*" }, true);

            }
            catch(const std::exception& e)
            {
                _this->importOpen = false;
                std::cerr << e.what() << '\n';
            }            
        }

        ImGui::TableSetColumnIndex(1);
        if (_this->selectedListName == "") { style::beginDisabled(); }  // selectedNames.size() == 0 && 
        if (ImGui::Button(("Експорт##supervisor_exp_4" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->exportOpen) {
            try
            {
                _this->exportedBookmarks = json::object();
                config.acquire();
                for (auto [bmName, bm] : config.conf["lists"][_this->selectedListName]["bookmarks"].items()) {
                    _this->exportedBookmarks["bookmarks"][bmName] = config.conf["lists"][_this->selectedListName]["bookmarks"][bmName];
                }
                config.release();
                _this->exportOpen = true;
                _this->exportDialog = new pfd::save_file("Export bookmarks", "export3_.json", { "JSON Files (*.json)", "*.json", "All Files", "*" }, true);
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << '\n';
            }
            
        }        
        if (_this->selectedListName == "") { style::endDisabled(); }  // selectedNames.size() == 0 && 
        ImGui::EndTable();

        //Draw buttons on top of the list
        if(_this->bookmarks_size>MAX_CHANNELS) {
            style::beginDisabled();                                     
        }          
        ImGui::BeginTable(("supervisor_btn_table" + _this->name).c_str(), 3);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);      

        if (ImGui::Button(("Додати##supervisor_add_4" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                flog::info("Додати....");
                char modName[1024];     
                _this->currDevList="";
                _this->currVCardDevList.clear();

                core::modComManager.callInterface("Аудіо вивід", AUDIO_SINK_GET_DEVLIST, NULL, &_this->txtDevList);            
                std::string dev;
                std::stringstream ss(_this->txtDevList);
                while (getline(ss, dev, '\0')){
                    int rez =-1;
                    for (auto& [name, bm] : _this->bookmarks) {
                        if(bm.scard == dev) {
                            rez = 1;
                            flog::info("Find. sound dev={0}", dev);
                            break;
                        }        
                    }    
                    if(rez==-1) {
                        _this->currVCardDevList.push_back(dev);
                        _this->currDevList= _this->currDevList + dev + '\0';
                        flog::info(">currDevList ={0} ", _this->currDevList);

                    }    
                    // _this->VCardDevList.push_back(dev);

                    flog::info("sound dev={0}", dev);

                }
                _this->devListId=0;
                 flog::info("sound _this->devListId ={0}, _this->currDevList ={1}, gui::waterfall.selectedVFO ={2} ", _this->devListId, _this->currDevList, gui::waterfall.selectedVFO);

                // If there's no VFO selected, just save the center freq
                
                _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
                _this->editedBookmark.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
                _this->editedBookmark.mode = 7;
                if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) != "") {
                    int mode;
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                    _this->editedBookmark.mode = mode;
                }
                _this->_frec = round(_this->editedBookmark.frequency);
                _this->_bandwidthId = -1;
                _this->_raw=false;
                for (int i = 0; i < _this->bandwidthsList.size(); i++) {
                    if (_this->bandwidthsList[i] >= _this->editedBookmark.bandwidth) {
                        _this->_bandwidthId = i;
                        break;
                    }
                }
                if(_this->_bandwidthId==-1) _this->_bandwidthId = _this->bandwidthsList.size();

                usleep(100);
                // Get FFT data
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

                flog::info("maxLevel = {0}", maxLevel);

                _this->editedBookmark.level = maxLevel;

                int curr_frequency = round(_this->editedBookmark.frequency);    
                _this->editedBookmark.scard="";
                // _this->editedBookmarkName =  std::to_string(curr_frequency);
                _this->editedBookmark.selected = false;

            // Find new unique default name
                if (_this->bookmarks.find("С1") == _this->bookmarks.end()) {
                    _this->editedBookmarkName = "С1";
                }
                else {
                    char buf[64];
                    for (int i = 2; i < 1000; i++) {
                        sprintf(buf, "С%d", i);
                        if (_this->bookmarks.find(buf) == _this->bookmarks.end()) { break; }
                    }
                    _this->editedBookmarkName = buf;
                }

                _this->createOpen = true; 
                    
        }
    
        if(_this->bookmarks_size>MAX_CHANNELS)  { style::endDisabled(); }       

        ImGui::TableSetColumnIndex(1);
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Видалити##supervisor_rem_4" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->deleteBookmarksOpen = true;
        }
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::TableSetColumnIndex(2);
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Редаг##supervisor_edt_4" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->editOpen = true;
            _this->currDevList = _this->txtDevList;
            _this->editedBookmark = _this->bookmarks[selectedNames[0]];
            _this->editedBookmarkName = selectedNames[0];
            _this->firstEditedBookmarkName = selectedNames[0];

            _this->_frec = round(_this->editedBookmark.frequency);
            _this->_bandwidthId = -1;
            _this->_raw=false;
            for (int i = 0; i < _this->bandwidthsList.size(); i++) {
                if (_this->bandwidthsList[i] >= _this->editedBookmark.bandwidth) {
                    _this->_bandwidthId = i;
                    break;
                }
            }
            if(_this->_bandwidthId==-1) _this->_bandwidthId = _this->bandwidthsList.size()-1;


            core::modComManager.callInterface("Аудіо вивід", 0, NULL, &_this->txtDevList);            

            std::string dev;
            std::stringstream ss(_this->txtDevList);
            _this->currDevList="";
//            _this->VCardDevList.clear();
            _this->currVCardDevList.clear();
            int _num_scard=0;
            while (getline(ss, dev, '\0')){
                int rez =-1;
                for (auto& [name, bm] : _this->bookmarks) {
                    if(bm.scard == dev) {
                        rez = 1;
                        flog::info("Find. sound dev={0}", dev);
                        break;
                    }        
                }    

                if(_this->editedBookmark.scard==dev) {
                    _this->devListId = _num_scard;
                }    

                if(rez==-1 || _this->editedBookmark.scard==dev) {
                    _this->currVCardDevList.push_back(dev);
                    _this->currDevList= _this->currDevList + dev + '\0';
                    _num_scard++;    
                }    
            }
            flog::info("sound _this->devListId ={0}, _this->currDevList ={1} ", _this->devListId, _this->currDevList);

        }
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::endDisabled(); }

        ImGui::EndTable();

        // Bookmark delete confirm dialog
        // List delete confirmation
        if (ImGui::GenericDialog(("supervisor_del_list_confirm4" + _this->name).c_str(), _this->deleteBookmarksOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::TextUnformatted("Видалення вибраних частот. Ви впевнені?");
            }) == GENERIC_DIALOG_BUTTON_YES) {
            for (auto& _name : selectedNames) { 
                _this->DelSelectedFreq(_name);
                _this->bookmarks.erase(_name);   
                _this->saveByName(_this->selectedListName);
                _this->refreshLists();
                _this->refreshWaterfallBookmarks(false);
            }        
        }
 
        // Bookmark list


        if (ImGui::BeginTable(("supervisor_bkm_table_4" + _this->name).c_str(), 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 100))) {
//            gui::waterfall.selectedVFO = "Канал приймання";            
            ImGui::TableSetupColumn("Назва");            
            ImGui::TableSetupColumn("Частота, МГц");
            ImGui::TableSetupColumn("Смуга, кГц");
            ImGui::TableSetupColumn("Поріг, дБ");
            ImGui::TableSetupColumn("Запис"); 
//            ImGui::TableSetupColumn("Звукова карта");
            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableHeadersRow();
            int i = 0;
            for (auto& [name, bm] : _this->bookmarks) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImVec2 min = ImGui::GetCursorPos();
                if (ImGui::Selectable((name + "##supervisor_bkm_name_4" + _this->name).c_str(), &bm.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick)) {
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
//                ImVec2 max = ImGui::GetCursorPos();

                ImGui::TableSetColumnIndex(2);
                int bw = round(bm.bandwidth/1000);
                std::string sbw = std::to_string(bw);
                if(bw==13) sbw ="12.5";
                if(bw==6) sbw ="6.25";
                ImGui::Text("%s",sbw.c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s",std::to_string(bm.level).c_str());

                ImGui::TableSetColumnIndex(4);
                if(_this->ch_recording[i]==true)
                        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Так");
                        // ImGui::Text("Так");
                else         
                        ImGui::TextColored(ImVec4(1, 0, 1, 1), "Стоп");
//                        ImGui::Text("Стоп");

//                ImGui::TableSetColumnIndex(5);
//                ImGui::Text("%s", bm.scard.c_str());

                ImVec2 max = ImGui::GetCursorPos();

                i++;
            }
            ImGui::EndTable();
        }

         bool _run = _this->running;
        //-----------------------------------------------------------------

        ImGui::Checkbox("Виставити загальний поріг для всіх частот##_supervision4__porig", &_this->flag_level);
        if (_run || gui::waterfall.selectedVFO!="Канал приймання" || _work>0) { ImGui::EndDisabled(); }

        if (!_this->flag_level) { ImGui::BeginDisabled(); }
        ImGui::LeftLabel("Поріг виявлення");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        ImGui::SliderFloat("##supervision4_level_3", &_this->level, -150.0, 0.0);
        if (!_this->flag_level) { ImGui::EndDisabled(); }

        //-----------------------------------------------------------------

      
        //-----------------------------------------------------------------       
        // if(gui::waterfall.selectedVFO!="Канал приймання") { style::beginDisabled(); }
        int _air_recording;
        core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);
        if (!_run) {
                if(_work>0)  ImGui::BeginDisabled(); 
                    if(_air_recording==1)  {
                        if (ImGui::Button("СТАРТ##supervision4_start_4", ImVec2(menuWidth, 0))) {
                           _this->start();
                        }
                    } else {
                        style::beginDisabled();
                        ImGui::Button("СТАРТ##supervision4_start_4", ImVec2(menuWidth, 0));
                        style::endDisabled();
                    }    
                if(_work>0)  ImGui::EndDisabled(); 
    
            ImGui::Text("Статус: Неактивний");
        }
        else {
            if(_air_recording==0)  {
                _this->stop();
            }            
            if (ImGui::Button("СТОП ##supervision4_start_4", ImVec2(menuWidth, 0))) {
                _this->stop();
            }
            if(_this->_recording == true) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Статус: Реєстрація");

            } else  {
                if(_run == true) {
                    ImGui::TextColored(ImVec4(1, 1, 0, 1), "Статус: Приймання");
                } 
                else {
                    ImGui::Text("Статус: Неактивний");
                }
            }
        }

        /// if(gui::waterfall.selectedVFO!="Канал приймання") { style::endDisabled(); }
        // if (_this->selectedListName == "") { style::endDisabled(); }

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
                    flog::info("1");

                    _this->importBookmarks(paths[0]);
                    flog::info("2");
            
                    _this->DelSelectedList();
                    config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = false;
                    _this->AddSelectedList();
                    config.conf["lists"][_this->selectedListName]["showOnWaterfall"] = true;
                    _this->refreshWaterfallBookmarks(false);
            
                }
                catch(const std::exception& e)
                {
                    // std::cerr << e.what() << '\n';
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
            flog::warn("_this->_error {0}", _this->_error);
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
        std::string folderPath = "%ROOT%/recordings";
        std::string expandedPath = expandString(folderPath + genLogFileName("/observ_"));
        /*
        logfile = std::ofstream(expandedPath.c_str(), std::ios::binary);
        if(logfile.is_open()){
            flog::info("Recording to '{0}'", expandedPath);
        }
        else {
            flog::error("Could not create '{0}'", expandedPath);
        }
        */
        int _air_recording;
        core::modComManager.callInterface("Airspy", AIRSPY_IFACE_CMD_GET_RECORDING, NULL, &_air_recording);
        flog::info("AIR Recording is '{0}'", _air_recording);
        if(_air_recording==1)  {
            running = true;
            // core::modComManager.callInterface("Airspy", AIRSPY_IFACE_CMD_START, NULL, NULL);
            workerThread = std::thread(&SupervisorModeModule::worker, this);
        }    

        core::modComManager.callInterface("Запис", MAIN_SET_START, NULL, NULL);

        bookmarks_size = bookmarks.size();
        itbook = bookmarks.begin();  
        _recording = false;
        
        curl = NULL;        
        try {
            curl = curl_easy_init();  
        }catch (...) {

        }        
        for(int i=0;i<MAX_CHANNELS;i++)
            ch_recording[i] = false;
    }

    void stop() {
        // flog::info("void stop(), running={0}", running);
        running = false;

        if (workerThread.joinable()) {
            workerThread.join();
        }

        core::modComManager.callInterface("Запис", MAIN_SET_STOP, NULL, NULL);

        std::string rec_name= "";
        if(_recording == true) {    
            int i=0;
            for (auto& [name, bm] : bookmarks) {
//                if(ch_recording[i]==true) {
                    rec_name = "Запис " + name;
                    flog::info("STOP Recording in name {0}", rec_name);
                    core::modComManager.callInterface(rec_name.c_str(), RECORDER_IFACE_CMD_STOP, NULL, NULL);
                    usleep(100);
                    ch_recording[i] = false;
//                }
                i++;
            }        
        }    
        _recording = false;    

        for(int i=0;i<MAX_CHANNELS;i++)
            ch_recording[i] = false;
        if (gui::waterfall.selectedVFO!="Канал приймання") {
                gui::waterfall.selectFirstVFO();
                //gui::waterfall.selectedVFO = "Канал приймання";
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
        double _lingerTime = _recordTime;
        // = _lingerTime;
        bool _Receiving = true;
        tuning = false;
        _detected = false;
//        std::string selectedVFO ="Канал приймання";
//        name = itbook->first;
//        auto bm =   itbook->second;         
//        bm.selected = true;
//        applyMode(bm, gui::waterfall.selectedVFO);                     
//        bookmarks.begin();
        if(itbook!=bookmarks.end()){ 
            name = itbook->first;
            ch_num = 0;       
            auto bm =   itbook->second;
            current = bm.frequency;    
            ch_rec_Name = "Запис " + name;
            ch_curr_selectedVFO = name;
            // curr_level = bm.level; 
            if(flag_level==true)
                curr_level = level;
            else                    
                curr_level = bm.level;
        } else 
            return;


        bool _r = false;
        for (size_t i = 0; i < MAX_CHANNELS; i++)
        {
            if(ch_recording[i]==true) {
                _r=true;
                flog::info("ch_recording[{0}]={1}, MAX_ch_num={2}, _recording = {3}", i, ch_recording[i], MAX_CHANNELS, _recording);                            
            }   
        }

        while (running) {

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            {
                std::lock_guard<std::mutex> lck(scan3Mtx);
                auto now = std::chrono::high_resolution_clock::now();
                     
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

                // Check if we are waiting for a tune                    
                 if (_Receiving) {
                    double vfoWidth = sigpath::vfoManager.getBandwidth(ch_curr_selectedVFO);
                    float maxLevel = getMaxLevel(data, current, vfoWidth, dataWidth, wfStart, wfWidth);
                    if (maxLevel >= curr_level) {
                        _count_rcv=4;                        
                        // flog::info("TRACE. Receiving... curr_level = {0}, maxLevel = {1}, current = {2}, _lingerTime {3}, _recording={4}, _record={5} !", curr_level, maxLevel, current, _lingerTime, _recording, _record);                        
                        // if(firstSignalTime==zirotime) {
                            firstSignalTime = now;
                            // if(_record==true && running==true && _recording==false){ 
                            if(ch_recording[ch_num]==false  && running==true)  {
                                //  flog::info("TRACE. START Receiving! curr_level = {0}, maxLevel = {1}, current = {2} !", curr_level, maxLevel, current);
                                int _mode;
                                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &_mode);                                
                                curr_nameWavFile = genWavFileName(current, _mode);
                                if(curr_nameWavFile!="")
                                    curlPOST_begin(curr_nameWavFile);

                                flog::info(" << START Recording {0} ({1}), curr_level = {2}, maxLevel = {3} !", ch_rec_Name, ch_num, curr_level, maxLevel);
                                // core::modComManager.callInterface(ch_rec_Name.c_str(), RECORDER_IFACE_CMD_START, NULL, NULL);
                                int recMode = 1;// RECORDER_MODE_AUDIO;
                                if(_mode==7) // RAW
                                    recMode = 0; //RECORDER_MODE_BASEBAND
                                core::modComManager.callInterface(ch_rec_Name.c_str(), RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);
                                core::modComManager.callInterface(ch_rec_Name.c_str(), RECORDER_IFACE_CMD_START, (void *) curr_nameWavFile.c_str(), NULL);

                                ch_recording[ch_num]=true;
                                _recording = true;        
                                

//                                usleep(100);
                            } else {
                               /// flog::info("TRACE. {0} ({1}), curr_level = {2}, maxLevel = {3} !", ch_rec_Name, ch_num, curr_level, maxLevel);
                            }  
                            /*
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
                            */

                            // flog::info("TRACE. START Receiving... curr_level = {0}, maxLevel = {1}, current = {2}, _lingerTime {3}, _recording={4}, _record={5} !", curr_level, maxLevel, current, _lingerTime, _recording, _record);                        
//                        }
                        ch_lastSignalTime[ch_num] = now;
                        _Receiving = false;                          
                        _detected = true; 
/*                        
                        if (status_stop) {
                            if(_recording==true) {
                                if((std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSignalTime)).count() > _waitingTime) {
                                    core::modComManager.callInterface(ch_rec_Name, RECORDER_IFACE_CMD_STOP, NULL, NULL);   
                                    _recording=false;   
                                }   
                            }
                        }  else {
                            if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - firstSignalTime)).count() > _lingerTime) { // = _recordTime
                                flog::info("TRACE. STOP Receiving! Next... curr_level = {0}, maxLevel = {1}, current = {2} !", curr_level, maxLevel, current);                           
                                if(_recording==true) 
                                    core::modComManager.callInterface(ch_rec_Name, RECORDER_IFACE_CMD_STOP, NULL, NULL);   
                                _recording=false;   
                                _detected = false;                            
                                _Receiving = false;                          
                            }
                        }                     
*/                        
                    }
                    else {
                        if(_count_rcv>3) {                            
                            if(_detected==false) {
                                if(ch_recording[ch_num]==true) {  
                                    if((std::chrono::duration_cast<std::chrono::milliseconds>(now - ch_lastSignalTime[ch_num] )).count() > _waitingTime) {
                                        core::modComManager.callInterface(ch_rec_Name, RECORDER_IFACE_CMD_STOP, NULL, NULL);   
                                        // flog::info("    STOP Receiving {0} ({1}) !", ch_rec_Name, ch_num);
                                        flog::info(" >> STOP Receiving {0}! {1} - selectedVFO, curr_level = {2}, maxLevel = {3}, _recording={4}, _detected={5} !", ch_num, ch_curr_selectedVFO, curr_level, maxLevel, ch_recording[ch_num], _detected);
                                        ch_recording[ch_num]=false;         
                                        if(curr_nameWavFile!="") {
                                            curlPOST_end(curr_nameWavFile);
                                            curr_nameWavFile ="";
                                        }                           
                                    }   
                                }    
                            }
                            _Receiving = false; 
                        } else 
                            _count_rcv++;
                    }
                }  
              
                if(_Receiving==false) {    
                    // flog::info(" ! ch_curr_selectedVFO {0}", ch_curr_selectedVFO);
                    // if(itbook!=bookmarks.end()) {
                    itbook = next(itbook);
                    ch_num++;
                    // }
                    if(itbook==bookmarks.end()){ 
                        itbook=bookmarks.begin();
                        MAX_ch_num = ch_num;
                        ch_num=0;
                    } 
                    int _r = false;
                    for (size_t i = 0; i < MAX_ch_num; i++)
                    {
                        if(ch_recording[i]==true) {
                            _r=true;
                            // flog::info("ch_recording[{0}]={1}, MAX_ch_num={2}, _recording = {3}", i, ch_recording[i], MAX_ch_num, _recording);                            
                        }   
                    }
                    _recording = _r;

                       
                    name = itbook->first;
                    auto bm =   itbook->second;
                    current = bm.frequency;    
                    ch_rec_Name = "Запис " + name;
                    ch_curr_selectedVFO = name;
                    
                    // curr_level = bm.level;
                    if(flag_level==true)
                        curr_level = level;
                    else                    
                        curr_level = bm.level;


                    if(!gui::waterfall.setCurrVFO(ch_curr_selectedVFO)) {
                        _Receiving = false;
                        if(ch_recording[ch_num]==true) 
                            core::modComManager.callInterface(ch_rec_Name, RECORDER_IFACE_CMD_STOP, NULL, NULL);
                        ch_recording[ch_num] = false;
                        //    _skip = true; 
                    } else {
                        _Receiving = true;
                    }

                     lastTuneTime = now;
                    
                    _detected = false;
                    
                    _count_rcv =0;
                    firstSignalTime = zirotime;                    
                }

                // Release FFT Data
                gui::waterfall.releaseLatestFFT();
            }
        }
        // flog::info("record = {0}, bm[bandwidth] = {1}, bm[mode] = {2}, _name = {3}", bm.frequency, bm.bandwidth, bm.mode, name);        
    }

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        SupervisorModeModule* _this = (SupervisorModeModule*)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }

        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP) {
            for (auto const bm : _this->waterfallBookmarks) {
                double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);

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
        SupervisorModeModule* _this = (SupervisorModeModule*)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }

        if (_this->mouseClickedInLabel) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                _this->mouseClickedInLabel = false;
            }
            gui::waterfall.inputHandled = true;
            return;
        }

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
        ImGui::Text("List: %s", hoveredBookmark.listName.c_str());
        ImGui::Text("Частота: %s", utils::formatFreq(hoveredBookmark.bookmark.frequency).c_str());
        ImGui::Text("Ширина Смуги: %s", utils::formatFreq(hoveredBookmark.bookmark.bandwidth).c_str());
        ImGui::Text("Режим: %s", demodModeList[hoveredBookmark.bookmark.mode]);
        ImGui::Text("Поріг виявлення: %s", utils::formatFreq(hoveredBookmark.bookmark.level).c_str());
        ImGui::Text("Звукова: %s", hoveredBookmark.bookmark.scard.c_str());

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
            fbm.scard = bm["scard"];
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
        SupervisorModeModule* _this = (SupervisorModeModule*)ctx;
        flog::info("moduleInterfaceHandler, name = {0} ",  _this->selectedListName);
        
        struct FreqData {
            int freq;
            int mode;
        } pFreqData;
        pFreqData =  *(static_cast<FreqData *>(in)); 
                // pFreqData.freq = _this->current;            
            int _mode = pFreqData.mode; //  *(int*)in;
            if (gui::waterfall.selectedVFO == "") {
                _this->editedBookmark.frequency = freq; 
                _this->editedBookmark.bandwidth = 0;
                _this->editedBookmark.mode = (int) _mode;
            }
            else {
                _this->editedBookmark.frequency = freq;
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
            _this->devListId = 0;
            _this->editedBookmark.scard = _this->currVCardDevList[_this->devListId];

            _this->editedBookmark.level = _this->level;

            _this->editedBookmark.selected = false;

            _this->createOpen = true;

            // Find new unique default name
            if (_this->bookmarks.find("Новий") == _this->bookmarks.end()) {
                _this->editedBookmarkName = "Новий";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    sprintf(buf, "Новий (%d)", i);
                    if (_this->bookmarks.find(buf) == _this->bookmarks.end()) { break; }
                }
                _this->editedBookmarkName = buf;
            }

                // If editing, delete the original one
//               if (editOpen) {
                _this->bookmarks.erase(_this->firstEditedBookmarkName);
//               }
                _this->bookmarks[_this->editedBookmarkName] = _this->editedBookmark;

                _this->saveByName(_this->selectedListName);
             
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
    int bookmarks_size = 0;

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
    int lingerTime = 4000.0;
    int _waitingTime = 1500;
    float level = -72.0;
    bool receiving = true;
    bool tuning = false;
    bool scanUp = true;
    bool reverseLock = false;
    bool _record = true;
    int  _recordTime = 1500;    
    bool _recording = false;
    bool _detected = false;
    int curr_level = -70;
    bool flag_level = false;
    std::string ch_rec_Name ="Запис";
    bool ch_recording[MAX_CHANNELS]; 
    std::string ch_curr_selectedVFO="Канал приймання"; 
    
    int ch_num =0; 
    int MAX_ch_num = 0;

    std::ofstream logfile;
    std::string root = (std::string)core::args["root"];

    int devCount;
    int devId = 0;
    int devListId = 0;
    int defaultDev = 0;
    int _count = 0;

//    std::chrono::time_point<std::chrono::high_resolution_clock> lastSignalTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> lastTuneTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> firstSignalTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> ch_lastSignalTime[MAX_CHANNELS];
    std::string txtDevList;            
    std::string currDevList;            
//    std::vector<std::string>VCardDevList;
    std::vector<std::string>currVCardDevList;

    std::string curr_nameWavFile = "";
    std::string txt_error="";
    bool _error = false;

    std::vector<uint32_t> bandwidthsList;
    int _frec = 0, _bandwidthId = 0;
    bool _raw = false;

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

    config.setPath(core::args["root"].s() + "/supervision_config.json");
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
    return new SupervisorModeModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SupervisorModeModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
