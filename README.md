# CircadianWallpaper

CircadianWallpaper automatically changes your desktop wallpaper based on real astronomical data for your location.

## 🚀 Quick Start

To run every time you login - Compile or move CircadianWallpaper.exe here:
   ```
   %APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\
   ```

## 🌟 Key Features

### Real Astronomical Data
- Seasonal Accuracy: Colors automatically adjust throughout the year as sunrise/sunset times shift
- Location-Sensitive: Uses your exact coordinates for solar calculations
- Quarter Minute Precision: Subtle changes in color and shade every quarter minute
- Regular Schedule: Automatically fetches fresh solar data each day
- Backup Coordinates: Manual coordinates serve as fallback if detection fails

## 🌅 Color Schedule

Colors change based on your location's actual solar times:

- Deep Night → Nearly black midnight blue
- Pre-Dawn → Dark purple
- Dawn → Purple-pink
- Sunrise → Yellow-peach
- Morning → Pale green
- Solar Noon → Sky blue
- Afternoon → Light blue/golden
- Sunset → Orange
- Civil Twilight → Dark slate blue
- Evening → Deep purple

## ⚙️ Optional Configuration

Edit `config.ini` to customize:

- Disable auto-detection to use manual backup coordinates
- Enable debug mode (shows you what the day's colors will look like in a two dimensional strip)
