#include "layerSettings.h"

#include <fstream>
#include <sys/stat.h>

#if defined(WIN32)
#include <direct.h>
#include <windows.h>
#endif

static const int kMaxCharsPerLine = 4096;
static const char* kEnvSettingsPath = "PERFHAUS_SETTINGS_PATH";

LayerSettings::LayerSettings()
{
    m_validSettingsFile = false;
}


// TODO: Make strings constants
// Make shared util function to get env vars

void LayerSettings::openSettingsFile(std::string layerName)
{
    m_layerName = layerName;

    if (m_layerName.length() == 0)
        return;

    // figure out the path
    std::string settingsFilePath;

    std::string envFilePath;
    envFilePath.clear();

#if defined (WIN32)

    int size = GetEnvironmentVariable(kEnvSettingsPath, NULL, 0);
    if (size > 0) {
        char *buffer = new char[size];
        GetEnvironmentVariable(kEnvSettingsPath, buffer, size);
        envFilePath = buffer;
        delete[] buffer;
    }
#else
    const char* settingsPath = getenv(kEnvSettingsPath);
    if (settingsPath)
        envFilePath = settingsPath;

#endif

    struct stat info;
    if (stat(envFilePath.c_str(), &info) == 0)
    {
        if (info.st_mode & S_IFDIR)
        {
            envFilePath += "/PerfHaus.cfg";
        }
        settingsFilePath = envFilePath;
    }
    else
    {
#if defined(WIN32)
        settingsFilePath = getenv("USERPROFILE");
#else
        settingsFilePath = getenv("HOME");
#endif
        settingsFilePath += "/VkPerfHaus/PerfHaus.cfg";
    }

    // open the file
    std::ifstream settingsFile;
    settingsFile.open(settingsFilePath);

    if (!settingsFile.good())
        return;

    char lineBuf[kMaxCharsPerLine];
    const size_t layerNameLen = m_layerName.length();

    // TODO: How will I record enabled 'deep APIs'?

    while (!settingsFile.eof())
    {
        settingsFile.getline(lineBuf, kMaxCharsPerLine);

        char option[512];
        char value[512];

        char *pComment;

        // discard any comments delimited by '#' in the line
        pComment = strchr(lineBuf, '#');
        if (pComment) *pComment = '\0';

        if (sscanf(lineBuf, " %511[^\n\t =] = %511[^\n \t]", option, value) == 2) {
            std::string optStr(option);
            std::string valStr(value);

            if (optStr.find(m_layerName) == 0)
            {
                size_t subOptLen = optStr.length() - (layerNameLen + 1);
                std::string optName = optStr.substr(layerNameLen + 1, subOptLen);
                m_settingMap[optName] = valStr;
            }

        }
    }

    m_validSettingsFile = true;
}


std::string LayerSettings::getOption(std::string optionName)
{
    std::unordered_map<std::string, std::string>::const_iterator it;

    if ((it = m_settingMap.find(optionName)) == m_settingMap.end())
        return "";
    else
        return it->second.c_str();
}
