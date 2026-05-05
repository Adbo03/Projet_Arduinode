import asyncio
import struct
import csv
import time
from collections import deque
from bleak import BleakClient, BleakScanner


DEVICE_NAME = "ADXL355Z" 
UUID_X = "00002a58-0000-1000-8000-00805f9b34fb"
UUID_Y = "00002a59-0000-1000-8000-00805f9b34fb"
UUID_Z = "00002a5a-0000-1000-8000-00805f9b34fb"

CSV_FILENAME = "adxl355_data.csv"

data_buffer = deque(maxlen=10)

current_data = {"x": 0.0, "y": 0.0, "z": 0.0}

def update_csv():
    """Modify the csv file with the 10 latest values of the buffer"""
    with open(CSV_FILENAME, mode='w', newline='') as file:
        writer = csv.writer(file)
        writer.writerow(["Timestamp", "X", "Y", "Z"]) 
        writer.writerows(data_buffer)

def notification_handler(sender, data):
    
    # Decoding 4 bytes data sent 
    val = struct.unpack('<f', data)[0]
    
    sender_uuid = sender.uuid
    
    if sender_uuid == UUID_X:
        current_data["x"] = val
    elif sender_uuid == UUID_Y:
        current_data["y"] = val
    elif sender_uuid == UUID_Z:
        current_data["z"] = val
        
        timestamp = time.strftime('%H:%M:%S') + f".{int(time.time() * 1000 % 1000):03d}"
        
        data_buffer.append([timestamp, current_data["x"], current_data["y"], current_data["z"]])
        
        update_csv()
        
        print(f"[{timestamp}] X:{current_data['x']:+.3f}g | Y:{current_data['y']:+.3f}g | Z:{current_data['z']:+.3f}g")

async def run():
    print(f"Recherche de la carte '{DEVICE_NAME}'...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME)
    
    if not device:
        print(f"Carte '{DEVICE_NAME}' introuvable. Vérifiez qu'elle est allumée et non connectée à un autre appareil.")
        return

    print(f"Carte trouvée ({device.address}). Connexion en cours...")
    
    async with BleakClient(device) as client:
        print("Connecté au BLE ! Configuration des notifications...")
        
        # Subscribing for each axis
        await client.start_notify(UUID_X, notification_handler)
        await client.start_notify(UUID_Y, notification_handler)
        await client.start_notify(UUID_Z, notification_handler)
        
        print(f"Collecte lancée. Les données sont mises à jour dans '{CSV_FILENAME}'.")
        print("Appuyez sur Ctrl+C pour arrêter.")
        
        try:
            # Infinite loop to keep the communication alive
            while True:
                await asyncio.sleep(1)
        except asyncio.exceptions.CancelledError:
            pass 
        finally:
            print("\nArrêt de la collecte...")
            await client.stop_notify(UUID_X)
            await client.stop_notify(UUID_Y)
            await client.stop_notify(UUID_Z)

if __name__ == "__main__":
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        print("\nProgramme terminé par l'utilisateur.")