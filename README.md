ESP32 Pi-Hole disable button
====================

The objective is to make a physical button which will disable the Pi-Hole add blocker for one minute.

# Secrets
This repo does not track the WiFi SSID, password, or the pi-hole API token. You will have to run `idf.py menuconfig` and enter these secrets manually in the "WiFi and Pi-Hole credentials" sub-menu
