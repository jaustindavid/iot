#! /usr/bin/env python3

import time
import json
import requests
import os

with open('response.txt') as json_data:
	data = json.load(json_data)
print(json.dumps(data, indent=2))

feels = { 35: 'HOT', 30: 'WARM', 
          25: 'COOL', 15: 'CHILLY', 10: 'COLD',
		  5: 'FREEZING', 0: 'FROZE', 
		  -100: 'FROSTBITE' }

def txlate(forecast_temp):
	for feel in sorted(feels.keys()):
		if forecast_temp < feel:
			return feels[feel]

# format: age;index:min,max;<repeat>
def dump(eighths):
	ret = f"{eighths['freshness']}" + ';'
	for i in range(7):
		ret = ret + f"{i}:{eighths[i]['min']:.1f},{eighths[i]['max']:.1f};"
	return ret


current_temp = data["currentConditions"]["feelslike"]
forecast_min = current_temp
forecast_max = current_temp

eighths = dict()

epoch_now = int(time.time())
for day in data['days']:
	for hour in day['hours']:
		dt = (hour['datetimeEpoch'] - epoch_now)
		dh = int((hour['datetimeEpoch'] - epoch_now)/3600)
		if dt >= 0 and dh < 24:
			if hour['feelslike'] > forecast_max:
				forecast_max = hour['feelslike']
			if hour['feelslike'] < forecast_min:
				forecast_min = hour['feelslike']
			if hour['precipprob'] > 50:
				cond = "RAINY"
			elif hour['precipprob'] > 20:
				cond = "CLOUDY"
			else:
				cond = "SUNNY"
			print(f"+{dh} / {dt}: {hour['datetimeEpoch']}: {hour['feelslike']} -> {txlate(hour['feelslike'])}; {cond}")
			eighth = int(dh / 3)
			if not eighth in eighths:
				eighths[eighth] = dict()
				eighths[eighth]['min'] = eighths[eighth]['max'] = hour['feelslike']
				eighths[eighth]['cond'] = cond
			else:
				eighths[eighth]['min'] = min(eighths[eighth]['min'], hour['feelslike'])
				eighths[eighth]['max'] = max(eighths[eighth]['max'], hour['feelslike'])
				if eighths[eighth]['cond'] == "SUNNY":
					eighths[eighth]['cond'] = cond
				elif cond == "RAINY":
					eighths[eighth]['cond'] = cond
print(f"Upcoming: {forecast_min}={txlate(forecast_min)}, {forecast_max}={txlate(forecast_max)}")

for eighth in eighths:
	if eighths[eighth]['min'] < 25:
		eighths[eighth]['temp'] = eighths[eighth]['min']
	else:
		eighths[eighth]['temp'] = eighths[eighth]['max']
	del eighths[eighth]['min']
	del eighths[eighth]['max']

eighths['freshness'] = int(time.time())
print(json.dumps(eighths))
# print(dump(eighths))
os.system(f'~/bin/particle publish -q --public weather \'{eighths}\'')
# os.system(f'~/bin/particle publish --public weather \'{dump(eighths)}\'')
