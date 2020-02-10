#pragma once

#include <string>
#include <unordered_map>


class LayerSettings {
public:
    LayerSettings();

    void openSettingsFile(std::string layerName);

    bool hasValidSettingsFile() { return m_validSettingsFile; }
    std::string getOption(std::string optionName);

private:
    std::unordered_map<std::string, std::string> m_settingMap;
    std::string m_layerName;
    bool m_validSettingsFile;
};