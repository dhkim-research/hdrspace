#ifndef MERGEHDR_DESKTOP_APP_DESKTOPAPP_H
#define MERGEHDR_DESKTOP_APP_DESKTOPAPP_H

#include <filesystem>
#include <string>

class DesktopApp {
public:
    DesktopApp();
    int run();

private:
    struct AppPaths {
        std::filesystem::path appRoot;
        std::filesystem::path uiIndex;
        std::filesystem::path profilesDir;
        std::filesystem::path settingsFile;
        std::filesystem::path mergehdr;
        std::filesystem::path tev;
        std::filesystem::path oiiotool;
        std::filesystem::path cacheDir;
    };

    AppPaths paths_;

    static std::filesystem::path executablePath();
    static std::filesystem::path findOnPath(const std::string &name);
    static std::string jsonEscape(const std::string &value);
    static std::string parseSingleStringArg(const std::string &req);
    static std::string urlDecode(const std::string &value);

    AppPaths resolvePaths() const;
    void reloadPaths();
    std::string fileUrl(const std::filesystem::path &path) const;
    std::string appInfoJson() const;
    std::string profileInfoFromQuery(const std::string &query) const;
    std::string saveSettingsFromQuery(const std::string &query);
    std::string chooseDialogFromQuery(const std::string &query) const;
    std::string startMergeFromQuery(const std::string &query) const;
    std::string pollMergeJobFromQuery(const std::string &query) const;
    std::string makePreviewFromQuery(const std::string &query) const;
    std::string runChartCellsFromQuery(const std::string &query) const;
    std::string runColorCalibrateFromQuery(const std::string &query) const;
    std::string runShutterFromQuery(const std::string &query) const;
    std::string runApertureFromQuery(const std::string &query) const;
    std::string openTevForPath(const std::string &pathText) const;
};

#endif
