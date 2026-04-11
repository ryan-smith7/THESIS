import json

# Base dashboard
dash = {
  "id": None,
  "uid": "gui-prac2",
  "title": "GUI Prac2 – Sensor Dashboard",
  "timezone": "browser",
  "schemaVersion": 36,
  "version": 1,
  "refresh": "5s",
  "panels": [
    {"type": "row", "title": "BME280 – Environmental", "collapsed": False, "gridPos": {"h":1,"w":24,"x":0,"y":0}},
    {"type": "timeseries","title":"Temperature (°C)","fieldConfig":{"defaults":{"unit":"celsius"}},"targets":[{"target":"BME280_Temperature"}],"gridPos":{"h":8,"w":8,"x":0,"y":1}},
    {"type": "timeseries","title":"Pressure (kPa)","fieldConfig":{"defaults":{"unit":"pressurekpa"}},"targets":[{"target":"BME280_Pressure"}],"gridPos":{"h":8,"w":8,"x":8,"y":1}},
    {"type": "timeseries","title":"Humidity (%)","fieldConfig":{"defaults":{"unit":"percent"}},"targets":[{"target":"BME280_Humidity"}],"gridPos":{"h":8,"w":8,"x":16,"y":1}},
    {"type": "row","title":"ENS160 – Air Quality","collapsed":False,"gridPos":{"h":1,"w":24,"x":0,"y":9}},
    {"type": "timeseries","title":"eCO₂ (ppm)","fieldConfig":{"defaults":{"unit":"ppm"}},"targets":[{"target":"ENS160_eCO2"}],"gridPos":{"h":8,"w":8,"x":0,"y":10}},
    {"type": "timeseries","title":"TVOC (ppb)","targets":[{"target":"ENS160_TVOC"}],"gridPos":{"h":8,"w":8,"x":8,"y":10}},
    {"type": "timeseries","title":"Air Quality Index","targets":[{"target":"ENS160_AQI"}],"gridPos":{"h":8,"w":8,"x":16,"y":10}},
    {"type": "row","title":"AS7343 – Spectral Channels","collapsed":False,"gridPos":{"h":1,"w":24,"x":0,"y":18}}
  ],
  "time":{"from":"now-15m","to":"now"},
  "timepicker":{"refresh_intervals":["1s","5s","10s","30s","1m"]},
  "templating":{"list":[]},
  "annotations":{"list":[]}
}

# Append spectral panels
y = 19
x_positions = [0, 8, 16]
wavelengths = [405, 425, 450, 475, 515, 550, 555, 600, 640, 690, 745, 855]
for i, wl in enumerate(wavelengths):
    dash["panels"].append({
        "type": "timeseries",
        "title": f"AS7343 λ={wl} nm",
        "fieldConfig":{"defaults":{"unit":"none"}},
        "targets":[{"target":f"AS7343_{wl}nm"}],
        "gridPos":{"h":8,"w":8,"x":x_positions[i%3],"y":y}
    })
    if (i+1)%3==0:
        y += 8

with open("sensor_dashboard_full.json","w") as f:
    json.dump(dash, f, indent=2)

print("✅ Created sensor_dashboard_full.json — ready to import in Grafana.")
