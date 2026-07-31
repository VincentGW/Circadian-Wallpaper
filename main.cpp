#include <Windows.h>
#include <SFML/Graphics.hpp>
#include <iostream>
#include <ctime>
#include <cmath>
#include <string>
#include <fstream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>
#include <memory>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

// Power management for wake-from-sleep detection
HWND g_hWnd = NULL;
bool g_justWokeUp = false;

struct Color {
    int r, g, b;
    Color(int red = 0, int green = 0, int blue = 0) : r(red), g(green), b(blue) {}
};

struct SolarTimes {
    double sunrise_hour;
    double sunset_hour;
    double solar_noon_hour;
    double civil_twilight_begin;
    double civil_twilight_end;
    bool valid = false;
    std::string fetch_date;
    std::string source; // "api", "cache", or "fallback"
};

struct SolarCache {
    std::vector<SolarTimes> days; // 8 days: today + next 7 days
    std::string last_updated;
};

struct Config {
    double latitude = 40.7128;   // Default: NYC
    double longitude = -74.0060;
    std::string location_name = "New York City";
    int update_interval_minutes = 1;
    bool debug_mode = false;
    bool auto_detect_location = true;
};

class CircadianWallpaper {
private:
    struct MonitorWindow {
        std::unique_ptr<sf::RenderWindow> window;
        sf::Sprite watermarkSprite;
        int x, y, width, height;
    };

    Config config;
    std::vector<MonitorWindow> monitors;
    sf::Texture watermarkTexture;
    bool hasWatermark;
    time_t lastAccentColorUpdate;
    sf::Shader watermarkShader;
    bool watermarkShaderLoaded;

    struct ColorPoint {
        double hour;
        Color color;
        std::string period;
    };

    std::vector<ColorPoint> todaysColors;
    SolarTimes todaysSolarTimes;
    SolarCache solarCache;
    std::string lastFetchDate;
    std::string currentPeriodCache;

    static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
        auto* monitors = reinterpret_cast<std::vector<MonitorWindow>*>(dwData);

        MONITORINFO info;
        info.cbSize = sizeof(MONITORINFO);
        GetMonitorInfo(hMonitor, &info);

        MonitorWindow mw;
        // Use work area to exclude taskbar
        mw.x = info.rcWork.left;
        mw.y = info.rcWork.top;
        mw.width = info.rcWork.right - info.rcWork.left;
        mw.height = info.rcWork.bottom - info.rcWork.top;

        monitors->push_back(std::move(mw));
        return TRUE;
    }

public:
    CircadianWallpaper() : hasWatermark(false), lastAccentColorUpdate(0), watermarkShaderLoaded(false) {
        std::cout << "Initializing CircadianWallpaper..." << std::endl;

        loadConfig();

        if (config.auto_detect_location) {
            autoDetectLocation();
        }

        // Load existing solar cache
        loadSolarCache();

        // Load watermark
        loadWatermark();

        // Load watermark inversion shader
        if (hasWatermark && sf::Shader::isAvailable()) {
            const std::string fragShader = R"(
uniform sampler2D texture;
uniform float invertAmount;
void main() {
    vec4 pixel = texture2D(texture, gl_TexCoord[0].xy);
    vec4 inverted = vec4(1.0 - pixel.r, 1.0 - pixel.g, 1.0 - pixel.b, pixel.a);
    gl_FragColor = gl_Color * mix(pixel, inverted, invertAmount);
}
)";
            watermarkShaderLoaded = watermarkShader.loadFromMemory(fragShader, sf::Shader::Fragment);
            if (!watermarkShaderLoaded) {
                std::cout << "Warning: Watermark inversion shader failed to compile" << std::endl;
            }
        }

        // Enumerate all monitors
        EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));

        std::cout << "Found " << monitors.size() << " monitor(s)" << std::endl;

        // Create a window for each monitor
        for (size_t i = 0; i < monitors.size(); i++) {
            auto& m = monitors[i];

            std::cout << "Monitor " << i << ": " << m.width << "x" << m.height
                      << " at (" << m.x << ", " << m.y << ")" << std::endl;

            // Create window for this monitor
            m.window = std::make_unique<sf::RenderWindow>(
                sf::VideoMode(m.width, m.height),
                "CircadianWallpaper",
                sf::Style::None
            );
            m.window->setFramerateLimit(60);
            m.window->setPosition(sf::Vector2i(m.x, m.y));

            // Hide from taskbar by setting as a tool window
            HWND hwnd = m.window->getSystemHandle();
            LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TOOLWINDOW);
            ShowWindow(hwnd, SW_HIDE);
            ShowWindow(hwnd, SW_SHOW);

            // Set up watermark sprite for this monitor if watermark exists
            if (hasWatermark) {
                m.watermarkSprite.setTexture(watermarkTexture);

                // Center watermark on this monitor
                sf::FloatRect bounds = m.watermarkSprite.getLocalBounds();
                m.watermarkSprite.setOrigin(bounds.width / 2.0f, bounds.height / 2.0f);
                m.watermarkSprite.setPosition(m.width / 2.0f, m.height / 2.0f);

                // Set transparency (10% opacity = 25/255)
                m.watermarkSprite.setColor(sf::Color(255, 255, 255, 25));
            }
        }

        std::cout << "\n=== CircadianWallpaper v3.0 - SFML Edition ===" << std::endl;
        std::cout << "=================================================" << std::endl;
        std::cout << "Location: " << config.location_name << " (" << config.latitude << ", " << config.longitude << ")" << std::endl;
        std::cout << "Update interval: " << config.update_interval_minutes << " minute(s)" << std::endl;
        std::cout << "Monitors: " << monitors.size() << std::endl;
        std::cout << "Solar cache: " << getSolarCachePath() << " (8-day storage)" << std::endl;

        logMessage("CircadianWallpaper v3.0 - SFML Edition (8-Day Cache)");
        logMessage("=================================================");
        logMessage("Location: " + config.location_name + " (" + std::to_string(config.latitude) + ", " + std::to_string(config.longitude) + ")");
        logMessage("Update interval: " + std::to_string(config.update_interval_minutes) + " minute(s)");
        logMessage("Monitors: " + std::to_string(monitors.size()));
        logMessage("Solar cache: " + getSolarCachePath() + " (8-day storage)");
    }

    ~CircadianWallpaper() {
        for (auto& m : monitors) {
            if (m.window && m.window->isOpen()) {
                m.window->close();
            }
        }
    }
    
    void setWindowsAccentColor(const Color& bgColor) {
        // Determine brightness to decide whether to blend with black or white
        int brightness = (bgColor.r + bgColor.g + bgColor.b) / 3;

        Color accentColor;
        float blendAmount = 0.15f; // 15% opacity overlay

        if (brightness > 128) {
            // Light background - overlay black at 15% opacity
            accentColor.r = (unsigned char)(bgColor.r * (1.0f - blendAmount));
            accentColor.g = (unsigned char)(bgColor.g * (1.0f - blendAmount));
            accentColor.b = (unsigned char)(bgColor.b * (1.0f - blendAmount));
        } else {
            // Dark background - overlay white at 15% opacity
            accentColor.r = (unsigned char)(bgColor.r * (1.0f - blendAmount) + 255 * blendAmount);
            accentColor.g = (unsigned char)(bgColor.g * (1.0f - blendAmount) + 255 * blendAmount);
            accentColor.b = (unsigned char)(bgColor.b * (1.0f - blendAmount) + 255 * blendAmount);
        }

        // Format the color as ABGR DWORD (Windows format)
        DWORD colorValue = 0xFF000000 | (accentColor.b << 16) | (accentColor.g << 8) | accentColor.r;

        // Update registry directly without restarting explorer
        HKEY hKey;

        // Set DWM accent color
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\DWM", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegSetValueExA(hKey, "AccentColor", 0, REG_DWORD, (const BYTE*)&colorValue, sizeof(DWORD));
            RegSetValueExA(hKey, "ColorizationColor", 0, REG_DWORD, (const BYTE*)&colorValue, sizeof(DWORD));
            RegSetValueExA(hKey, "ColorizationAfterglow", 0, REG_DWORD, (const BYTE*)&colorValue, sizeof(DWORD));
            RegSetValueExA(hKey, "AccentColorMenu", 0, REG_DWORD, (const BYTE*)&colorValue, sizeof(DWORD));

            // Create accent palette (8 color variations)
            BYTE palette[32];
            for (int i = 0; i < 8; i++) {
                int factor = 100 + (i - 4) * 10;
                palette[i * 4 + 0] = std::min(255, std::max(0, (accentColor.r * factor) / 100));
                palette[i * 4 + 1] = std::min(255, std::max(0, (accentColor.g * factor) / 100));
                palette[i * 4 + 2] = std::min(255, std::max(0, (accentColor.b * factor) / 100));
                palette[i * 4 + 3] = 255;
            }
            RegSetValueExA(hKey, "AccentPalette", 0, REG_BINARY, palette, 32);

            RegCloseKey(hKey);
        }

        // Notify system of color change
        SendMessageTimeout(HWND_BROADCAST, WM_DWMCOLORIZATIONCOLORCHANGED, colorValue, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
        SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)"ImmersiveColorSet", SMTO_ABORTIFHUNG, 1000, nullptr);

        logMessage("Accent color updated: RGB(" + std::to_string(accentColor.r) + ", " + std::to_string(accentColor.g) + ", " + std::to_string(accentColor.b) + ")");
    }

    std::string getConfigPath() {
        // Get the directory where the executable is located
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exeDir = std::string(exePath);
        size_t lastSlash = exeDir.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            exeDir = exeDir.substr(0, lastSlash + 1);
        } else {
            exeDir = "";
        }
        
        // Go up one directory level
        size_t parentSlash = exeDir.find_last_of("\\/", exeDir.length() - 2);
        std::string parentDir;
        if (parentSlash != std::string::npos) {
            parentDir = exeDir.substr(0, parentSlash + 1);
        } else {
            parentDir = exeDir; // Fallback to current directory if parent not found
        }
        
        return parentDir + "config.ini";
    }
    
    void loadConfig() {
        std::string configPath = getConfigPath();
        std::ifstream configFile(configPath);
        if (configFile.is_open()) {
            std::string line;
            while (std::getline(configFile, line)) {
                if (line.empty() || line[0] == '#') continue;
                
                size_t equalPos = line.find('=');
                if (equalPos != std::string::npos) {
                    std::string key = line.substr(0, equalPos);
                    std::string value = line.substr(equalPos + 1);
                    
                    // Trim whitespace
                    key.erase(0, key.find_first_not_of(" \t"));
                    key.erase(key.find_last_not_of(" \t") + 1);
                    value.erase(0, value.find_first_not_of(" \t"));
                    value.erase(value.find_last_not_of(" \t") + 1);
                    
                    if (key == "latitude") config.latitude = std::stod(value);
                    else if (key == "longitude") config.longitude = std::stod(value);
                    else if (key == "location_name") config.location_name = value;
                    else if (key == "update_interval_minutes") config.update_interval_minutes = std::stoi(value);
                    else if (key == "debug_mode") config.debug_mode = (value == "true");
                    else if (key == "auto_detect_location") config.auto_detect_location = (value == "true");
                }
            }
            configFile.close();
            if (config.debug_mode) logMessage("Config loaded from config.ini");
        } else {
            createDefaultConfig();
        }
    }
    
    void createDefaultConfig() {
        std::string configPath = getConfigPath();
        std::ofstream configFile(configPath);
        if (configFile.is_open()) {
            configFile << "# CircadianWallpaper Configuration" << std::endl;
            configFile << "# auto_detect_location=true: Automatically detect your location via IP geolocation" << std::endl;
            configFile << "# auto_detect_location=false: Use manual coordinates below as primary location" << std::endl;
            configFile << "# Manual coordinates also serve as backup if auto-detection fails" << std::endl;
            configFile << "# Get coordinates from: https://www.latlong.net/" << std::endl;
            configFile << std::endl;
            configFile << "latitude=" << config.latitude << std::endl;
            configFile << "longitude=" << config.longitude << std::endl;
            configFile << "location_name=" << config.location_name << std::endl;
            configFile << "update_interval_minutes=" << config.update_interval_minutes << std::endl;
            configFile << "debug_mode=" << (config.debug_mode ? "true" : "false") << std::endl;
            configFile << "auto_detect_location=" << (config.auto_detect_location ? "true" : "false") << std::endl;
            configFile.close();
            logMessage("Created default config.ini - location will be auto-detected!");
        }
    }
    
    std::string httpGetWithTimeout(const std::string& url, int timeoutMs = 5000) {
        HINTERNET hInternet = InternetOpenA("CircadianWallpaper/2.0", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
        if (!hInternet) return "";
        
        // Set timeouts
        DWORD timeout = timeoutMs;
        InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionA(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
        
        HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(), nullptr, 0, 
                                         INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (!hUrl) {
            InternetCloseHandle(hInternet);
            return "";
        }
        
        std::string response;
        char buffer[4096];
        DWORD bytesRead;
        
        while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            response.append(buffer, bytesRead);
        }
        
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return response;
    }
    
    bool detectLocationFromIP() {
        if (config.debug_mode) logMessage("Trying IP geolocation...");
        
        std::string response = httpGetWithTimeout("http://ip-api.com/json/?fields=status,lat,lon,city,regionName,country", 8000);
        
        if (response.empty()) {
            if (config.debug_mode) logMessage("IP geolocation failed - no response");
            return false;
        }
        
        if (response.find("\"status\":\"success\"") == std::string::npos) {
            if (config.debug_mode) logMessage("IP geolocation failed - invalid response");
            return false;
        }
        
        try {
            // Parse latitude
            size_t latPos = response.find("\"lat\":");
            if (latPos != std::string::npos) {
                size_t startPos = latPos + 6;
                size_t endPos = response.find_first_of(",}", startPos);
                if (endPos != std::string::npos) {
                    std::string latStr = response.substr(startPos, endPos - startPos);
                    config.latitude = std::stod(latStr);
                }
            }
            
            // Parse longitude
            size_t lonPos = response.find("\"lon\":");
            if (lonPos != std::string::npos) {
                size_t startPos = lonPos + 6;
                size_t endPos = response.find_first_of(",}", startPos);
                if (endPos != std::string::npos) {
                    std::string lonStr = response.substr(startPos, endPos - startPos);
                    config.longitude = std::stod(lonStr);
                }
            }
            
            // Parse city name
            size_t cityPos = response.find("\"city\":\"");
            if (cityPos != std::string::npos) {
                size_t startPos = cityPos + 8;
                size_t endPos = response.find("\"", startPos);
                if (endPos != std::string::npos) {
                    std::string city = response.substr(startPos, endPos - startPos);
                    
                    // Also get region for better location name
                    size_t regionPos = response.find("\"regionName\":\"");
                    if (regionPos != std::string::npos) {
                        size_t regionStart = regionPos + 14;
                        size_t regionEnd = response.find("\"", regionStart);
                        if (regionEnd != std::string::npos) {
                            std::string region = response.substr(regionStart, regionEnd - regionStart);
                            config.location_name = city + ", " + region;
                        } else {
                            config.location_name = city;
                        }
                    } else {
                        config.location_name = city;
                    }
                }
            }
            
            if (config.debug_mode) {
                logMessage("IP geolocation successful:");
                logMessage("  Location: " + config.location_name);
                logMessage("  Coordinates: " + std::to_string(config.latitude) + ", " + std::to_string(config.longitude));
            }
            
            return true;
            
        } catch (...) {
            if (config.debug_mode) logMessage("IP geolocation failed - parsing error");
            return false;
        }
    }
    
    bool autoDetectLocation() {
        if (!config.auto_detect_location) {
            if (config.debug_mode) logMessage("Auto-detection disabled in config");
            return false;
        }
        
        if (config.debug_mode) logMessage("Auto-detecting location...");
        
        // Try IP geolocation first (most reliable)
        if (detectLocationFromIP()) {
            saveLocationToConfig();
            return true;
        }
        
        if (config.debug_mode) logMessage("All location detection methods failed");
        return false;
    }
    
    void saveLocationToConfig() {
        // Update the config file with detected location
        std::string configPath = getConfigPath();
        std::ofstream configFile(configPath);
        if (configFile.is_open()) {
            configFile << "# CircadianWallpaper Configuration" << std::endl;
            configFile << "# Location auto-detected successfully!" << std::endl;
            configFile << "# Set auto_detect_location=false to use manual coordinates as primary location." << std::endl;
            configFile << "# Manual coordinates below serve as backup if auto-detection fails." << std::endl;
            configFile << std::endl;
            configFile << "latitude=" << config.latitude << std::endl;
            configFile << "longitude=" << config.longitude << std::endl;
            configFile << "location_name=" << config.location_name << std::endl;
            configFile << "update_interval_minutes=" << config.update_interval_minutes << std::endl;
            configFile << "debug_mode=" << (config.debug_mode ? "true" : "false") << std::endl;
            configFile << "auto_detect_location=" << (config.auto_detect_location ? "true" : "false") << std::endl;
            configFile.close();
            
            if (config.debug_mode) logMessage("Location saved to config.ini");
        }
    }
    
    double parseTimeString(const std::string& timeStr) {
        if (timeStr.length() < 19) return 0.0;
        
        std::string timeOnly = timeStr.substr(11, 8);
        int hours, minutes, seconds;
        char colon1, colon2;
        
        std::stringstream ss(timeOnly);
        if (ss >> hours >> colon1 >> minutes >> colon2 >> seconds) {
            double utcHour = hours + (minutes / 60.0) + (seconds / 3600.0);
            
            // Get current system timezone info to determine if DST is active
            TIME_ZONE_INFORMATION tzi;
            DWORD tziResult = GetTimeZoneInformation(&tzi);
            
            double offsetHours;
            if (tziResult == TIME_ZONE_ID_DAYLIGHT) {
                // DST is active - use daylight bias
                offsetHours = -(tzi.Bias + tzi.DaylightBias) / 60.0;
            } else {
                // Standard time - use standard bias
                offsetHours = -(tzi.Bias + tzi.StandardBias) / 60.0;
            }
            
            double localHour = utcHour + offsetHours;
            if (localHour < 0) localHour += 24.0;
            if (localHour >= 24) localHour -= 24.0;
            
            return localHour;
        }
        
        return 0.0;
    }
    
    double parseTimeFromJson(const std::string& json, const std::string& key) {
        size_t keyPos = json.find(key);
        if (keyPos == std::string::npos) return 0.0;
        
        size_t startQuote = json.find("\"", keyPos + key.length());
        if (startQuote == std::string::npos) return 0.0;
        startQuote++;
        
        size_t endQuote = json.find("\"", startQuote);
        if (endQuote == std::string::npos) return 0.0;
        
        std::string timeStr = json.substr(startQuote, endQuote - startQuote);
        return parseTimeString(timeStr);
    }
    
    bool fetchSolarTimesForDate(const std::string& targetDate, SolarTimes& solarTimes, bool isRetry = false) {
        std::stringstream urlBuilder;
        urlBuilder << "https://api.sunrise-sunset.org/json?lat=" << config.latitude
                   << "&lng=" << config.longitude << "&date=" << targetDate << "&formatted=0";

        std::string url = urlBuilder.str();
        if (config.debug_mode) logMessage("Fetching solar times for " + targetDate + "..." + std::string(isRetry ? " (retry)" : ""));

        std::string response = httpGetWithTimeout(url, isRetry ? 10000 : 5000);

        if (response.empty()) {
            if (!isRetry) {
                if (config.debug_mode) logMessage("API request timed out, retrying once...");
                return fetchSolarTimesForDate(targetDate, solarTimes, true);
            }
            if (config.debug_mode) logMessage("API Error: Request timed out for " + targetDate);
            return false;
        }

        if (response.find("\"status\":\"OK\"") == std::string::npos) {
            if (config.debug_mode) logMessage("API Error: Invalid response for " + targetDate);
            return false;
        }

        try {
            solarTimes.sunrise_hour = parseTimeFromJson(response, "\"sunrise\":");
            solarTimes.sunset_hour = parseTimeFromJson(response, "\"sunset\":");
            solarTimes.solar_noon_hour = parseTimeFromJson(response, "\"solar_noon\":");
            solarTimes.civil_twilight_begin = parseTimeFromJson(response, "\"civil_twilight_begin\":");
            solarTimes.civil_twilight_end = parseTimeFromJson(response, "\"civil_twilight_end\":");

            if (solarTimes.sunrise_hour > 0 && solarTimes.sunset_hour > 0) {
                solarTimes.valid = true;
                solarTimes.fetch_date = targetDate;
                solarTimes.source = "api";

                if (config.debug_mode) {
                    logMessage("Solar times fetched successfully for " + targetDate + ":");
                    logMessage("  Sunrise: " + formatHour(solarTimes.sunrise_hour));
                    logMessage("  Sunset: " + formatHour(solarTimes.sunset_hour));
                }

                return true;
            }
        } catch (...) {
            if (config.debug_mode) logMessage("JSON parsing error for " + targetDate);
        }

        return false;
    }

    bool fetchEightDaySolarData() {
        // Initialize cache with 8 days if not already sized
        if (solarCache.days.size() != 8) {
            solarCache.days.resize(8);
        }

        int successCount = 0;

        // Fetch data for today and next 7 days (8 days total)
        for (int dayOffset = 0; dayOffset < 8; dayOffset++) {
            std::string targetDate = getDateOffset(dayOffset);
            if (fetchSolarTimesForDate(targetDate, solarCache.days[dayOffset])) {
                successCount++;
            }
        }

        if (successCount > 0) {
            solarCache.last_updated = getCurrentDate();
            saveSolarCache();
            logMessage("Updated solar cache with " + std::to_string(successCount) + "/8 days from API");
            return true;
        }

        logMessage("Failed to fetch any solar data from API");
        return false;
    }

    SolarTimes* findCachedDataForDate(const std::string& targetDate) {
        for (size_t i = 0; i < solarCache.days.size(); i++) {
            if (solarCache.days[i].fetch_date == targetDate) {
                return &solarCache.days[i];
            }
        }
        return nullptr;
    }

    bool shouldUpdateCache() {
        std::string today = getCurrentDate();

        // Only update once per day - if we already updated today, don't update again
        if (solarCache.last_updated == today) {
            return false;
        }

        return true;
    }

    bool fetchSolarTimes(bool forceRefresh = false) {
        std::string today = getCurrentDate();

        // Load cache first
        loadSolarCache();

        // Check if we have valid cached data for today
        SolarTimes* todaysData = findCachedDataForDate(today);
        if (!forceRefresh && todaysData && todaysData->valid) {
            todaysSolarTimes = *todaysData;
            if (config.debug_mode) logMessage("Using cached solar times for " + today + " (source: " + todaysData->source + ")");
            return true;
        }

        // Only attempt API fetch if we haven't already updated today
        if (shouldUpdateCache()) {
            logMessage("Attempting daily solar data update for " + today + " (8 days)");
            if (fetchEightDaySolarData()) {
                // Use today's data if available
                todaysData = findCachedDataForDate(today);
                if (todaysData && todaysData->valid) {
                    todaysSolarTimes = *todaysData;
                    return true;
                }
            }
        } else {
            if (config.debug_mode) logMessage("Skipping API update - already updated today");
        }

        // Fall back to cached data if available (any cached data for today)
        todaysData = findCachedDataForDate(today);
        if (todaysData && todaysData->valid) {
            todaysSolarTimes = *todaysData;
            logMessage("Using cached solar times for " + today + " (source: " + todaysData->source + ")");
            return true;
        }

        // Check for any available cached data as backup (search all 8 days)
        for (size_t i = 0; i < solarCache.days.size(); i++) {
            if (solarCache.days[i].valid) {
                todaysSolarTimes = solarCache.days[i];
                todaysSolarTimes.fetch_date = today;
                todaysSolarTimes.source = "cache-backup-day" + std::to_string(i);
                logMessage("Using cached solar times from day " + std::to_string(i) + " as backup for " + today);
                return true;
            }
        }

        // Last resort: use January averages
        logMessage("No cached data available. Using fallback times.");
        return useFallbackSolarTimes();
    }
    
    bool useFallbackSolarTimes() {
        todaysSolarTimes.sunrise_hour = 7.2;
        todaysSolarTimes.sunset_hour = 17.1;
        todaysSolarTimes.solar_noon_hour = 12.15;
        todaysSolarTimes.civil_twilight_begin = 6.7;
        todaysSolarTimes.civil_twilight_end = 17.6;
        todaysSolarTimes.valid = false;
        todaysSolarTimes.fetch_date = getCurrentDate();
        todaysSolarTimes.source = "fallback";

        logMessage("Using fallback solar times (NYC January average)");
        return false;
    }
    
    std::string getCurrentDate() {
        time_t now = time(0);
        tm* timeinfo = localtime(&now);

        std::stringstream ss;
        ss << (timeinfo->tm_year + 1900) << "-"
           << std::setfill('0') << std::setw(2) << (timeinfo->tm_mon + 1) << "-"
           << std::setw(2) << timeinfo->tm_mday;
        return ss.str();
    }

    std::string getDateOffset(int dayOffset) {
        time_t now = time(0);
        now += dayOffset * 24 * 60 * 60; // Add/subtract days
        tm* timeinfo = localtime(&now);

        std::stringstream ss;
        ss << (timeinfo->tm_year + 1900) << "-"
           << std::setfill('0') << std::setw(2) << (timeinfo->tm_mon + 1) << "-"
           << std::setw(2) << timeinfo->tm_mday;
        return ss.str();
    }

    std::string getSolarCachePath() {
        std::string configPath = getConfigPath();
        size_t lastSlash = configPath.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            return configPath.substr(0, lastSlash + 1) + "solar_cache.txt";
        }
        return "solar_cache.txt";
    }

    void saveSolarCache() {
        std::string cachePath = getSolarCachePath();
        std::ofstream cacheFile(cachePath);
        if (!cacheFile.is_open()) {
            if (config.debug_mode) logMessage("Failed to save solar cache to " + cachePath);
            return;
        }

        cacheFile << "# CircadianWallpaper Solar Cache - Eight Day Data (Today + Next 7 Days)" << std::endl;
        cacheFile << "last_updated=" << solarCache.last_updated << std::endl;
        cacheFile << std::endl;

        // Save all 8 days of data
        for (size_t i = 0; i < solarCache.days.size(); i++) {
            const SolarTimes& dayData = solarCache.days[i];

            cacheFile << "[day" << i << "]" << std::endl;
            cacheFile << "date=" << dayData.fetch_date << std::endl;
            cacheFile << "sunrise=" << std::fixed << std::setprecision(6) << dayData.sunrise_hour << std::endl;
            cacheFile << "sunset=" << std::fixed << std::setprecision(6) << dayData.sunset_hour << std::endl;
            cacheFile << "solar_noon=" << std::fixed << std::setprecision(6) << dayData.solar_noon_hour << std::endl;
            cacheFile << "civil_twilight_begin=" << std::fixed << std::setprecision(6) << dayData.civil_twilight_begin << std::endl;
            cacheFile << "civil_twilight_end=" << std::fixed << std::setprecision(6) << dayData.civil_twilight_end << std::endl;
            cacheFile << "valid=" << (dayData.valid ? "true" : "false") << std::endl;
            cacheFile << "source=" << dayData.source << std::endl;
            cacheFile << std::endl;
        }

        cacheFile.close();
        if (config.debug_mode) logMessage("Solar cache saved successfully (8 days)");
    }

    bool loadSolarCache() {
        std::string cachePath = getSolarCachePath();
        std::ifstream cacheFile(cachePath);
        if (!cacheFile.is_open()) {
            if (config.debug_mode) logMessage("No existing solar cache found");
            return false;
        }

        // Initialize with 8 empty entries
        solarCache.days.clear();
        solarCache.days.resize(8);

        std::string line;
        int currentDayIndex = -1;

        while (std::getline(cacheFile, line)) {
            if (line.empty() || line[0] == '#') continue;

            // Check for day sections [day0], [day1], etc.
            if (line.substr(0, 4) == "[day" && line.back() == ']') {
                std::string dayStr = line.substr(4, line.length() - 5);
                currentDayIndex = std::stoi(dayStr);
                if (currentDayIndex < 0 || currentDayIndex >= 8) {
                    currentDayIndex = -1; // Invalid index
                }
                continue;
            }

            size_t equalPos = line.find('=');
            if (equalPos != std::string::npos) {
                std::string key = line.substr(0, equalPos);
                std::string value = line.substr(equalPos + 1);

                if (key == "last_updated") {
                    solarCache.last_updated = value;
                } else if (currentDayIndex >= 0 && currentDayIndex < 8) {
                    SolarTimes& currentTimes = solarCache.days[currentDayIndex];
                    if (key == "date") currentTimes.fetch_date = value;
                    else if (key == "sunrise") currentTimes.sunrise_hour = std::stod(value);
                    else if (key == "sunset") currentTimes.sunset_hour = std::stod(value);
                    else if (key == "solar_noon") currentTimes.solar_noon_hour = std::stod(value);
                    else if (key == "civil_twilight_begin") currentTimes.civil_twilight_begin = std::stod(value);
                    else if (key == "civil_twilight_end") currentTimes.civil_twilight_end = std::stod(value);
                    else if (key == "valid") currentTimes.valid = (value == "true");
                    else if (key == "source") currentTimes.source = value;
                }
            }
        }

        cacheFile.close();
        if (config.debug_mode) logMessage("Solar cache loaded successfully (8 days)");
        return true;
    }
    
    void generateTodaysColors() {
        if (config.debug_mode) {
            logMessage("Solar-based continuous color calculation initialized");
            logMessage("  Sunrise: " + formatHour(todaysSolarTimes.sunrise_hour));
            logMessage("  Solar Noon: " + formatHour(todaysSolarTimes.solar_noon_hour));
            logMessage("  Sunset: " + formatHour(todaysSolarTimes.sunset_hour));
            logMessage("  Civil Twilight End: " + formatHour(todaysSolarTimes.civil_twilight_end));
        }
    }
    
    void generateDebugCSV() {
        std::string csvPath = getConfigPath();
        size_t lastSlash = csvPath.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            csvPath = csvPath.substr(0, lastSlash + 1) + "daily_colors.csv";
        } else {
            csvPath = "daily_colors.csv";
        }
        
        std::ofstream csvFile(csvPath);
        if (!csvFile.is_open()) {
            logMessage("Failed to create debug CSV file: " + csvPath);
            return;
        }
        
        // Write CSV header
        csvFile << "Time,Hour,Minute,R,G,B,Period" << std::endl;
        
        // Generate colors for each minute of the day
        for (int hour = 0; hour < 24; hour++) {
            for (int minute = 0; minute < 60; minute++) {
                double timeHour = hour + (minute / 60.0);
                std::string period;
                Color color = getColorForHour(timeHour, &period);

                // Format time as HH:MM
                std::stringstream timeStr;
                timeStr << std::setfill('0') << std::setw(2) << hour
                       << ":" << std::setw(2) << minute;

                csvFile << timeStr.str() << ","
                       << std::fixed << std::setprecision(2) << timeHour << ","
                       << minute << ","
                       << color.r << "," << color.g << "," << color.b << ","
                       << period << std::endl;
            }
        }
        
        csvFile.close();
        logMessage("Debug CSV generated: " + csvPath);
    }
    
    
    Color getCurrentColor() {
        time_t now = time(0);
        tm* timeinfo = localtime(&now);
        // Include seconds for 15-second granularity
        double currentHour = timeinfo->tm_hour + (timeinfo->tm_min / 60.0) + (timeinfo->tm_sec / 3600.0);

        return getColorForHour(currentHour);
    }
    
    
    Color getColorForHour(double hour, std::string* outPeriod = nullptr) {
        double sunrise = todaysSolarTimes.sunrise_hour;
        double sunset = todaysSolarTimes.sunset_hour;
        double solar_noon = todaysSolarTimes.solar_noon_hour;
        double civil_twilight_end = todaysSolarTimes.civil_twilight_end;

        // Normalize hour to 0-24 range
        while (hour < 0) hour += 24.0;
        while (hour >= 24) hour -= 24.0;

        // Create base timeline points ensuring no overlaps
        std::vector<ColorPoint> points;
        
        // Night and early morning
        points.push_back({0.0, Color(7, 8, 26), "Deep Night"});
        points.push_back({std::max(1.0, sunrise - 3.0), Color(8, 9, 27), "Pre-Dawn"});
        points.push_back({std::max(2.0, sunrise - 1.5), Color(18, 21, 53), "Early Dawn"});
        points.push_back({std::max(3.0, sunrise - 1.0), Color(25, 26, 65), "Early Dawn"});
        points.push_back({std::max(4.0, sunrise - 0.5), Color(50, 54, 75), "Dawn"});
        points.push_back({std::max(5.0, sunrise - 0.25), Color(65, 68, 100), "Dawn"});
        
        // Sunrise and morning
        points.push_back({std::max(6.0, sunrise), Color(210, 120, 70), "Sunrise"});
        points.push_back({std::max(7.0, sunrise + 0.25), Color(200, 210, 150), "Sunrise"});
        points.push_back({std::max(8.0, sunrise + 0.5), Color(190, 210, 200), "Early Morning"});
        points.push_back({std::max(9.0, sunrise + 1.0), Color(180, 210, 210), "Early Morning"});
        points.push_back({std::max(10.0, sunrise + 2.0), Color(175, 210, 220), "Morning"});
        
        // Day time
        points.push_back({std::max(11.0, solar_noon - 1.5), Color(170, 210, 230), "Late Morning"});
        points.push_back({std::max(11.5, solar_noon - 1.0), Color(170, 210, 230), "Late Morning"});
        points.push_back({std::max(12.0, solar_noon), Color(170, 210, 230), "Noon"});
        points.push_back({std::max(13.0, solar_noon + 1.0), Color(170, 210, 230), "Early Afternoon"});
        points.push_back({std::max(14.0, solar_noon + 1.5), Color(170, 210, 230), "Early Afternoon"});
        
        // Afternoon to sunset - ensure progressive timing
        double late_afternoon_start = sunset - 2.0;
        double pre_sunset_start = sunset - 1.5;
        double pre_sunset_mid = sunset - 1.0;
        double pre_sunset_end = sunset - 0.5;
        double sunset_time = sunset;
        
        points.push_back({late_afternoon_start, Color(170, 210, 230), "Late Afternoon"});
        points.push_back({pre_sunset_start, Color(170, 210, 230), "Late Afternoon"});
        points.push_back({pre_sunset_mid, Color(175, 200, 225), "Pre-Sunset"});
        points.push_back({pre_sunset_end, Color(180, 195, 220), "Pre-Sunset"});
        points.push_back({sunset_time, Color(230, 140, 70), "Sunset"});
        
        // Post-sunset to evening - ensure monotonic progression
        double post_sunset_1 = sunset_time + 0.1;
        double post_sunset_2 = post_sunset_1 + 0.1;
        double post_sunset_3 = post_sunset_2 + 0.1;
        double twilight_1 = post_sunset_3 + 0.1;
        double twilight_2 = twilight_1 + 0.1;
        double twilight_3 = twilight_2 + 0.1;
        
        points.push_back({post_sunset_1, Color(200, 120, 70), "Sunset"});
        points.push_back({post_sunset_2, Color(145, 100, 80), "Post-Sunset"});
        points.push_back({post_sunset_3, Color(110, 90, 87), "Post-Sunset"});
        points.push_back({twilight_1, Color(85, 80, 95), "Civil Twilight"});
        points.push_back({twilight_2, Color(55, 70, 101), "Civil Twilight"});
        points.push_back({twilight_3, Color(45, 58, 89), "Civil Twilight"});
        
        // Evening progression - based on twilight end
        double evening_start = twilight_3 + 0.1;
        points.push_back({evening_start, Color(40, 43, 70), "Evening"});
        points.push_back({evening_start + 0.25, Color(26, 33, 60), "Evening"});
        points.push_back({evening_start + 0.5, Color(22, 25, 57), "Evening"});
        points.push_back({evening_start + 0.75, Color(18, 21, 53), "Evening"});
        points.push_back({evening_start + 1.0, Color(13, 15, 43), "Evening"});
        points.push_back({evening_start + 1.25, Color(12, 14, 39), "Evening"});
        points.push_back({evening_start + 1.5, Color(11, 12, 35), "Evening"});
        points.push_back({evening_start + 1.75, Color(10, 11, 31), "Late Evening"});
        points.push_back({evening_start + 2.25, Color(9, 10, 28), "Late Evening"});
        points.push_back({evening_start + 2.75, Color(8, 9, 27), "Late Evening"});
        points.push_back({evening_start + 3.25, Color(7, 8, 26), "Night"});
        points.push_back({23.99, Color(7, 8, 26), "Night"});
        
        // Fix times outside 0-24 range
        for (auto& point : points) {
            while (point.hour < 0) point.hour += 24.0;
            while (point.hour >= 24) point.hour -= 24.0;
        }
        
        // Sort points by time
        std::sort(points.begin(), points.end(), 
                  [](const ColorPoint& a, const ColorPoint& b) {
                      return a.hour < b.hour;
                  });
        
        // Find the appropriate color interpolation
        for (size_t i = 0; i < points.size() - 1; i++) {
            if (hour >= points[i].hour && hour <= points[i + 1].hour) {
                double progress = (hour - points[i].hour) / (points[i + 1].hour - points[i].hour);
                if (outPeriod) *outPeriod = points[i].period;
                return interpolateColor(points[i].color, points[i + 1].color, progress);
            }
        }

        // Handle wrap-around (from last point to first point)
        double lastHour = points.back().hour;
        double firstHour = points.front().hour + 24;

        if (hour >= lastHour) {
            double progress = (hour - lastHour) / (firstHour - lastHour);
            if (outPeriod) *outPeriod = points.back().period;
            return interpolateColor(points.back().color, points.front().color, progress);
        }

        if (outPeriod) *outPeriod = points.front().period;
        return points.front().color;
    }
    
    bool isTimeBetween(double current, double start, double end) {
        // Handle case where time range wraps around midnight
        if (start > end) {
            return (current >= start || current <= end);
        }
        return (current >= start && current <= end);
    }
    
    double getProgressBetween(double current, double start, double end) {
        if (start > end) {
            // Handle wrap around midnight
            if (current >= start) {
                return (current - start) / (24.0 - start + end);
            } else {
                return (24.0 - start + current) / (24.0 - start + end);
            }
        }
        return (current - start) / (end - start);
    }
    
    std::string getCurrentPeriod() {
        time_t now = time(0);
        tm* timeinfo = localtime(&now);
        double currentHour = timeinfo->tm_hour + (timeinfo->tm_min / 60.0);

        std::string period;
        getColorForHour(currentHour, &period);
        return period;
    }
    
    Color interpolateColor(Color start, Color end, double ratio) {
        ratio = std::max(0.0, std::min(1.0, ratio));
        
        int r = static_cast<int>(start.r * (1 - ratio) + end.r * ratio);
        int g = static_cast<int>(start.g * (1 - ratio) + end.g * ratio);
        int b = static_cast<int>(start.b * (1 - ratio) + end.b * ratio);
        
        return Color(r, g, b);
    }
    
    std::string formatHour(double hour) {
        int h = static_cast<int>(hour);
        int m = static_cast<int>((hour - h) * 60);
        
        std::string ampm = "AM";
        if (h == 0) h = 12;
        else if (h == 12) ampm = "PM";
        else if (h > 12) { h -= 12; ampm = "PM"; }
        
        std::stringstream ss;
        ss << h << ":" << std::setfill('0') << std::setw(2) << m << " " << ampm;
        return ss.str();
    }
    
    void loadWatermark() {
        std::string configPath = getConfigPath();
        size_t lastSlash = configPath.find_last_of("\\/");
        std::string watermarkPath;
        if (lastSlash != std::string::npos) {
            watermarkPath = configPath.substr(0, lastSlash + 1) + "Watermark.png";
        } else {
            watermarkPath = "Watermark.png";
        }

        if (watermarkTexture.loadFromFile(watermarkPath)) {
            hasWatermark = true;
            std::cout << "Watermark loaded successfully" << std::endl;
            logMessage("Watermark loaded successfully");
        } else {
            hasWatermark = false;
            std::cout << "No watermark found at: " << watermarkPath << std::endl;
            logMessage("No watermark found at: " + watermarkPath);
        }
    }

    float computeWatermarkInvertProgress() {
        double sunrise = todaysSolarTimes.sunrise_hour;
        double sunset  = todaysSolarTimes.sunset_hour;

        const double transitionHours = 2.0 / 60.0; // 2 minutes transition time for smooth fade
        const double offsetHours     = 1;         // 60 minutes after sunset

        double invertStart = sunset + offsetHours;
        double invertFull  = invertStart + transitionHours;
        double revertFull  = sunrise - offsetHours;
        double revertStart = revertFull - transitionHours;

        time_t now = time(0);
        tm* ti = localtime(&now);
        double h = ti->tm_hour + ti->tm_min / 60.0 + ti->tm_sec / 3600.0;

        // Transitioning to inverted (evening, shortly after sunset+90min)
        if (h >= invertStart && h < invertFull) {
            return (float)((h - invertStart) / transitionHours);
        }
        // Fully inverted — wraps midnight: after invertFull in the evening OR before revertStart in the morning
        if (h >= invertFull || h < revertStart) {
            return 1.0f;
        }
        // Transitioning back to normal (early morning, before sunrise-90min)
        if (h >= revertStart && h < revertFull) {
            return (float)(1.0 - (h - revertStart) / transitionHours);
        }
        // Daytime — normal
        return 0.0f;
    }

    void renderDebugStrip() {
        float invertProgress = computeWatermarkInvertProgress();
        for (auto& m : monitors) {
            if (m.window && m.window->isOpen()) {
                const int stripHeight = 300;
                const int stripY = (m.height - stripHeight) / 2;

                sf::Image stripImage;
                stripImage.create(m.width, m.height, sf::Color(10, 10, 20));

                for (int x = 0; x < m.width; x++) {
                    double hour = (static_cast<double>(x) / m.width) * 24.0;
                    Color c = getColorForHour(hour);
                    sf::Color px(c.r, c.g, c.b);

                    int yEnd = std::min(stripY + stripHeight, m.height);
                    for (int y = stripY; y < yEnd; y++) {
                        stripImage.setPixel(x, y, px);
                    }
                }

                sf::Texture stripTexture;
                stripTexture.loadFromImage(stripImage);
                sf::Sprite stripSprite(stripTexture);

                m.window->clear(sf::Color(10, 10, 20));
                m.window->draw(stripSprite);
                if (hasWatermark) {
                    if (watermarkShaderLoaded) {
                        watermarkShader.setUniform("texture", sf::Shader::CurrentTexture);
                        watermarkShader.setUniform("invertAmount", invertProgress);
                        m.window->draw(m.watermarkSprite, &watermarkShader);
                    } else {
                        m.window->draw(m.watermarkSprite);
                    }
                }
                m.window->display();
            }
        }
    }

    void renderFrame(const Color& bgColor) {
        if (config.debug_mode) {
            renderDebugStrip();
            return;
        }

        float invertProgress = computeWatermarkInvertProgress();

        // Get current time and calculate future time (15 minutes ahead)
        time_t now = time(0);
        tm* timeinfo = localtime(&now);
        double currentHour = timeinfo->tm_hour + (timeinfo->tm_min / 60.0) + (timeinfo->tm_sec / 3600.0);
        double timeSpan = 0.25; // 15 minutes = 0.25 hours

        // 8x8 Bayer matrix for ordered dithering
        static const int bayerMatrix[8][8] = {
            { 0, 32,  8, 40,  2, 34, 10, 42},
            {48, 16, 56, 24, 50, 18, 58, 26},
            {12, 44,  4, 36, 14, 46,  6, 38},
            {60, 28, 52, 20, 62, 30, 54, 22},
            { 3, 35, 11, 43,  1, 33,  9, 41},
            {51, 19, 59, 27, 49, 17, 57, 25},
            {15, 47,  7, 39, 13, 45,  5, 37},
            {63, 31, 55, 23, 61, 29, 53, 21}
        };

        // Render to all monitor windows
        for (auto& m : monitors) {
            if (m.window && m.window->isOpen()) {
                // Create dithered gradient using per-pixel rendering
                sf::Image gradientImage;
                gradientImage.create(m.width, m.height);

                for (int y = 0; y < m.height; y++) {
                    // Calculate vertical progress (0.0 at bottom, 1.0 at top)
                    double verticalProgress = 1.0 - (static_cast<double>(y) / m.height);

                    // Calculate the time at this vertical position (linearly interpolate from current to future time)
                    double timeAtThisRow = currentHour + (verticalProgress * timeSpan); // verticalProgress goes from 0 (bottom/future) to 1 (top/current)

                    // Get the actual color for this specific time from the precalculated color schedule
                    Color baseColor = getColorForHour(timeAtThisRow);

                    // Get adjacent color for dithering calculation
                    double nextRowTime = currentHour + ((verticalProgress + (1.0 / m.height)) * timeSpan);
                    Color nextColor = getColorForHour(nextRowTime);

                    for (int x = 0; x < m.width; x++) {
                        // Get dither threshold from Bayer matrix (0-63)
                        int bayerValue = bayerMatrix[y % 8][x % 8];
                        double threshold = (bayerValue / 64.0) - 0.5; // Range: -0.5 to ~0.5

                        // Apply dithering: add threshold scaled by color difference between adjacent rows
                        Color colorDiff;
                        colorDiff.r = nextColor.r - baseColor.r;
                        colorDiff.g = nextColor.g - baseColor.g;
                        colorDiff.b = nextColor.b - baseColor.b;

                        int r = std::max(0, std::min(255, baseColor.r + static_cast<int>(threshold * abs(colorDiff.r) * 0.5)));
                        int g = std::max(0, std::min(255, baseColor.g + static_cast<int>(threshold * abs(colorDiff.g) * 0.5)));
                        int b = std::max(0, std::min(255, baseColor.b + static_cast<int>(threshold * abs(colorDiff.b) * 0.5)));

                        gradientImage.setPixel(x, y, sf::Color(r, g, b));
                    }
                }

                // Create texture from image and draw it
                sf::Texture gradientTexture;
                gradientTexture.loadFromImage(gradientImage);
                sf::Sprite gradientSprite(gradientTexture);

                m.window->clear();
                m.window->draw(gradientSprite);

                // Draw watermark if available
                if (hasWatermark) {
                    if (watermarkShaderLoaded) {
                        watermarkShader.setUniform("texture", sf::Shader::CurrentTexture);
                        watermarkShader.setUniform("invertAmount", invertProgress);
                        m.window->draw(m.watermarkSprite, &watermarkShader);
                    } else {
                        m.window->draw(m.watermarkSprite);
                    }
                }

                m.window->display();
            }
        }
    }

    void updateDisplay() {
        Color currentColor = getCurrentColor();
        renderFrame(currentColor);
    }
    
    void createMessageWindow() {
        // Create a hidden window to receive power management messages
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = PowerEventWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "CircadianWallpaperPowerWindow";
        RegisterClassA(&wc);
        
        g_hWnd = CreateWindowA("CircadianWallpaperPowerWindow", "CircadianWallpaper Power", 
                              0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandle(NULL), this);
    }

    static LRESULT CALLBACK PowerEventWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (uMsg == WM_POWERBROADCAST) {
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                g_justWokeUp = true;
                // Get the CircadianWallpaper instance from window data
                CircadianWallpaper* instance = (CircadianWallpaper*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
                if (instance) {
                    instance->logMessage("System resumed from sleep - updating wallpaper immediately");
                }
            }
        }
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }

    void run() {
        std::cout << "\nStarting CircadianWallpaper..." << std::endl;
        std::cout << "Display will update every 15 seconds for smooth color transitions" << std::endl;
        std::cout << "Use Task Manager to terminate the application" << std::endl << std::endl;

        logMessage("Starting CircadianWallpaper...");
        logMessage("Display will update every 15 seconds for smooth color transitions");

        // Create hidden window for power management messages
        createMessageWindow();
        if (g_hWnd) {
            SetWindowLongPtr(g_hWnd, GWLP_USERDATA, (LONG_PTR)this);
            std::cout << "Power management enabled - will update on wake from sleep" << std::endl;
            logMessage("Power management enabled - will update on wake from sleep");
        }

        // Initial setup
        std::cout << "Fetching solar times..." << std::endl;
        fetchSolarTimes();
        std::cout << "Generating color schedule..." << std::endl;
        generateTodaysColors();

        int updateCount = 0;
        std::string lastDate = getCurrentDate();
        sf::Clock updateClock;

        // Do initial render and set initial accent color
        std::cout << "Rendering initial frame..." << std::endl;
        updateDisplay();
        updateCount++;

        Color initialColor = getCurrentColor();
        setWindowsAccentColor(initialColor);
        lastAccentColorUpdate = time(0);

        std::cout << "\nEntering main loop..." << std::endl;

        bool shouldRun = true;
        while (shouldRun) {
            // Handle SFML events for all windows
            for (auto& m : monitors) {
                if (m.window && m.window->isOpen()) {
                    sf::Event event;
                    while (m.window->pollEvent(event)) {
                        if (event.type == sf::Event::Closed) {
                            shouldRun = false;
                        }
                        // No keyboard shortcuts - use Task Manager to terminate
                    }
                }
            }

            // Process Windows messages for power events
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            
            try {
                // Check if we just woke up from sleep
                bool forceUpdate = false;
                if (g_justWokeUp) {
                    logMessage("Wake from sleep detected - checking cached data first");

                    // Try to use cached data before forcing API refresh
                    std::string today = getCurrentDate();
                    loadSolarCache();

                    SolarTimes* todaysData = findCachedDataForDate(today);
                    if (todaysData && todaysData->valid) {
                        todaysSolarTimes = *todaysData;
                        logMessage("Using cached solar data after wake (source: " + todaysData->source + ")");
                    } else {
                        // Use any available cached data as backup from the 8-day cache
                        bool foundBackup = false;
                        for (size_t i = 0; i < solarCache.days.size(); i++) {
                            if (solarCache.days[i].valid) {
                                todaysSolarTimes = solarCache.days[i];
                                todaysSolarTimes.fetch_date = today;
                                todaysSolarTimes.source = "cache-wake-day" + std::to_string(i);
                                logMessage("Using cached data from day " + std::to_string(i) + " after wake as backup");
                                foundBackup = true;
                                break;
                            }
                        }

                        if (!foundBackup) {
                            // No cached data available, try to fetch fresh data
                            logMessage("No cached data available after wake - attempting fresh fetch");
                            fetchSolarTimes(true); // Force refresh
                        }
                    }

                    forceUpdate = true;
                    g_justWokeUp = false;
                }
                
                // Check if we need to refresh solar times (new day)
                std::string currentDate = getCurrentDate();
                if (currentDate != lastDate) {
                    logMessage("New day detected, refreshing solar times...");
                    fetchSolarTimes();
                    generateTodaysColors();
                    lastDate = currentDate;
                    forceUpdate = true;
                }

                // Check if it's time for periodic update (every 15 seconds)
                if (updateClock.getElapsedTime().asSeconds() >= 15.0f) {
                    forceUpdate = true;
                    updateClock.restart();
                }

                // Always render, but only log on updates
                updateDisplay();
                if (forceUpdate) {
                    updateCount++;

                    if (config.debug_mode || updateCount % 1 == 0) { // Show status updates
                        time_t now = time(0);
                        tm* timeinfo = localtime(&now);
                        Color currentColor = getCurrentColor();
                        std::string period = getCurrentPeriod();

                        std::string statusMsg = "[" + std::to_string(updateCount) + "] "
                                               + formatHour(timeinfo->tm_hour + (timeinfo->tm_min / 60.0))
                                               + " | " + period
                                               + " | RGB(" + std::to_string(currentColor.r) + ", "
                                               + std::to_string(currentColor.g) + ", "
                                               + std::to_string(currentColor.b) + ")"
                                               + " | Source: " + todaysSolarTimes.source;

                        std::cout << statusMsg << std::endl;
                        logMessage(statusMsg);
                    }
                }

                // Check if it's time to update Windows accent color (every 30 minutes)
                time_t now = time(0);
                if (difftime(now, lastAccentColorUpdate) >= 1800) { // 30 minutes = 1800 seconds
                    Color currentColor = getCurrentColor();
                    setWindowsAccentColor(currentColor);
                    lastAccentColorUpdate = now;
                }

                // Small sleep to prevent high CPU usage
                std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60fps
                
            } catch (const std::exception& e) {
                std::string errorMsg = "Error in main loop: " + std::string(e.what());
                logMessage(errorMsg);
                std::this_thread::sleep_for(std::chrono::minutes(1));
            }
        }
    }

    void logMessage(const std::string& message) {
        // Logging disabled to prevent large log files with 15-second updates
        // Re-enable this function if debugging is needed
        (void)message; // Suppress unused parameter warning
    }

};

int main(int argc, char* argv[]) {
    // Set DPI awareness to prevent scaling issues on high-DPI displays
    SetProcessDPIAware();

    CircadianWallpaper app;
    
    if (argc > 1) {
        std::string mode = argv[1];
        if (mode == "--help" || mode == "-h") {
            std::cout << "\nUsage:" << std::endl;
            std::cout << "  CircadianWallpaper.exe              - Run fullscreen color overlay based on solar position" << std::endl;
            std::cout << "  CircadianWallpaper.exe --help       - Show this help" << std::endl;
            std::cout << "\nFeatures:" << std::endl;
            std::cout << "  • Fullscreen SFML overlay (fast, no wallpaper API calls)" << std::endl;
            std::cout << "  • Automatic location detection via IP geolocation" << std::endl;
            std::cout << "  • Real astronomical data for your location" << std::endl;
            std::cout << "  • Real-time color transitions based on sun position" << std::endl;
            std::cout << "  • Transparent watermark support (Watermark.png)" << std::endl;
            std::cout << "  • Wake from sleep detection - updates immediately on resume" << std::endl;
            std::cout << "  • All output is logged to log.txt file" << std::endl;
            std::cout << "\nControls:" << std::endl;
            std::cout << "  ESC - Exit application" << std::endl;
            std::cout << "\nEdit config.ini to set manual location coordinates if needed." << std::endl;
            return 0;
        }
    }
    
    app.run();
    return 0;
}