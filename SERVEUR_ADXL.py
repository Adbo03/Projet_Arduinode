import asyncio
import threading
import struct
from collections import deque
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from bleak import BleakClient, BleakScanner

# --- CONFIGURATION ---
DEVICE_NAME = "ADXL355Z" 
UUID_X = "00002a58-0000-1000-8000-00805f9b34fb"
UUID_Y = "00002a59-0000-1000-8000-00805f9b34fb"
UUID_Z = "00002a5a-0000-1000-8000-00805f9b34fb"

BUFFER_SIZE = 50
x_data = deque([0.0]*BUFFER_SIZE, maxlen=BUFFER_SIZE)
y_data = deque([0.0]*BUFFER_SIZE, maxlen=BUFFER_SIZE)
z_data = deque([0.0]*BUFFER_SIZE, maxlen=BUFFER_SIZE)

# --- 1. BLE (Background) ---
def notification_handler(sender, data):
    """Reçoit les données BLE et met à jour les buffers en RAM directement."""
    val = struct.unpack('<f', data)[0]
    sender_uuid = sender.uuid.lower()
    
    if sender_uuid == UUID_X:
        x_data.append(val)
    elif sender_uuid == UUID_Y:
        y_data.append(val)
    elif sender_uuid == UUID_Z:
        z_data.append(val)

async def run_ble():
    print(f"Recherche de la carte '{DEVICE_NAME}'...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME)

    if not device:
        print("Carte introuvable. Fermez la fenêtre graphique pour quitter.")
        return

    async with BleakClient(device) as client:
        print("Connecté au BLE ! Démarrage du flux de données...")
        await client.start_notify(UUID_X, notification_handler)
        await client.start_notify(UUID_Y, notification_handler)
        await client.start_notify(UUID_Z, notification_handler)
        
        # Infinite loop to keep the connection alive
        try:
            while True:
                await asyncio.sleep(1)
        except asyncio.CancelledError:
            pass
        finally:
            await client.stop_notify(UUID_X)
            await client.stop_notify(UUID_Y)
            await client.stop_notify(UUID_Z)
            await client.disconnect()

def start_ble_thread():
    """Lance la boucle asynchrone BLE dans un Thread séparé."""
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(run_ble())

# --- 2. Display (Matplotlib) ---
fig, ax = plt.subplots()
ax.set_title("ADXL355 Accéléromètre - Temps Réel")
ax.set_ylim(-3.0, 3.0)  # Y scale between -3g and +3g
ax.set_ylabel("Accélération (g)")
ax.set_xlim(0, BUFFER_SIZE)

line_x, = ax.plot(x_data, label='Axe X', color='red')
line_y, = ax.plot(y_data, label='Axe Y', color='green')
line_z, = ax.plot(z_data, label='Axe Z', color='blue')
ax.legend(loc='upper right')

def update_plot(frame):
    """Mise à jour périodique des courbes avec le contenu actuel du buffer."""
    line_x.set_ydata(x_data)
    line_y.set_ydata(y_data)
    line_z.set_ydata(z_data)
    return line_x, line_y, line_z

# --- 3. MAIN  ---
if __name__ == "__main__":

    # Starting the BLE in the background
    ble_thread = threading.Thread(target=start_ble_thread, daemon=True)
    ble_thread.start()

    # Starting the display window (update every 50ms)
    ani = animation.FuncAnimation(fig, update_plot, interval=50, blit=True, cache_frame_data=False)
    
    plt.show() 
    
    print("Fermeture du programme...")