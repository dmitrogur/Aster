#pragma once
#include <string>
#include <ctime>
#include <chrono>

namespace utils {
    std::string formatFreq(double freq) {
        char str[128];
        if (freq >= 1000000.0) {
            sprintf(str, "%.06lf", freq / 1000000.0);
            int len = strlen(str) - 1;
            while ((str[len] == '0' || str[len] == '.') && len > 0) {
                len--;
                if (str[len] == '.') {
                    len--;
                    break;
                }
            }
            return std::string(str).substr(0, len + 1) + "МГц";
        }
        else if (freq >= 1000.0) {
            sprintf(str, "%.06lf", freq / 1000.0);
            int len = strlen(str) - 1;
            while ((str[len] == '0' || str[len] == '.') && len > 0) {
                len--;
                if (str[len] == '.') {
                    len--;
                    break;
                }
            }
            return std::string(str).substr(0, len + 1) + "КГц";
        }
        else {
            sprintf(str, "%.06lf", freq);
            int len = strlen(str) - 1;
            while ((str[len] == '0' || str[len] == '.') && len > 0) {
                len--;
                if (str[len] == '.') {
                    len--;
                    break;
                }
            }
            return std::string(str).substr(0, len + 1) + "Гц";
        }
    }

    std::string formatFreqMHz(double freq) {
        char str[128];
        if (freq >= 1000000.0) {
            sprintf(str, "%.06lf", freq / 1000000.0);
            int len = strlen(str) - 1;
            while ((str[len] == '0' || str[len] == '.') && len > 0) {
                len--;
                if (str[len] == '.') {
                    len--;
                    break;
                }
            }
            return std::string(str).substr(0, len + 1);
        }
        else if (freq >= 1000.0) {
            sprintf(str, "%.06lf", freq / 1000.0);
            int len = strlen(str) - 1;
            while ((str[len] == '0' || str[len] == '.') && len > 0) {
                len--;
                if (str[len] == '.') {
                    len--;
                    break;
                }
            }
            return std::string(str).substr(0, len + 1);
        }
        else {
            sprintf(str, "%.06lf", freq);
            int len = strlen(str) - 1;
            while ((str[len] == '0' || str[len] == '.') && len > 0) {
                len--;
                if (str[len] == '.') {
                    len--;
                    break;
                }
            }
            return std::string(str).substr(0, len + 1);
        }
    }  

    uint64_t timeSinceEpochMillisec() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    std::string unixTimestamp(){
        time_t now = time(0);
        tm* ltm = localtime(&now);
        uint64_t ms = timeSinceEpochMillisec();
        ms = ms - (now*1000);
        return std::to_string(now)+"."+std::to_string(ms);
    }

}