#pragma once

enum {
    RADIO_IFACE_CMD_GET_MODE,
    RADIO_IFACE_CMD_SET_MODE,
    RADIO_IFACE_CMD_GET_BANDWIDTH,
    RADIO_IFACE_CMD_SET_BANDWIDTH,
    RADIO_IFACE_CMD_GET_SQUELCH_ENABLED,
    RADIO_IFACE_CMD_SET_SQUELCH_ENABLED,
    RADIO_IFACE_CMD_GET_SQUELCH_LEVEL,
    RADIO_IFACE_CMD_SET_SQUELCH_LEVEL,
};

enum {
    RADIO_IFACE_MODE_NFM,
    RADIO_IFACE_MODE_WFM,
    RADIO_IFACE_MODE_AM,
    RADIO_IFACE_MODE_DSB,
    RADIO_IFACE_MODE_USB,
    RADIO_IFACE_MODE_CW,
    RADIO_IFACE_MODE_LSB,
    RADIO_IFACE_MODE_RAW
};

const char* demodModeList[] = {
    "NFM",
    "WFM",
    "AM",
    "DSB",
    "USB",
    "CW",
    "LSB",
    "RAW"
};

const char* demodModeListTxt = "NFM\0WFM\0AM\0DSB\0USB\0CW\0LSB\0RAW\0";

const char* bandListTxt = " 1000\0 6250\0 12500\0 25000\0 50000\0 100000\0 220000\0";
