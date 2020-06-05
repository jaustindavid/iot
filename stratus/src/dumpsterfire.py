import datetime
import time
from secrets import DUMPSTERFIRE_SECRET, DUMPSTERFIRE_MQ
import stratus_client
import os


def callback(key, value):
    print(f'calledback: {key} :: {value}')

def main():
    stratus = stratus_client.StratusClient(DUMPSTERFIRE_MQ, DUMPSTERFIRE_SECRET, f"simple_client" )
    stratus.subscribe(callback, "fuel", "dumpsterfire", reset=False, limit=1, debuggery=True)
    stratus.update(debuggery = True)

if __name__ == "__main__":
    main()
