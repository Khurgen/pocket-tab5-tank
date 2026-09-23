# Pocket Tab5 Tank v0.1.0
# Run Once launcher for UIFlow2

import sys

APP_DIR = "/flash/pocket_tab5_tank"
MODULE_NAME = "app_engine"

if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

try:
    if MODULE_NAME in sys.modules:
        del sys.modules[MODULE_NAME]
except:
    pass

import app_engine as app

app.setup()

while True:
    app.loop()
