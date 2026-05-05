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
UUID_PITCH = "00002a5b-0000-1000-8000-00805f9b34fb"
UUID_ROLL = "00002a5c-0000-1000-8000-00805f9b34fb"

BUFFER_SIZE = 50
x_data = deque([0.0]*BUFFER_SIZE, maxlen=BUFFER_SIZE)
y_data = deque([0.0]*BUFFER_SIZE, maxlen=BUFFER_SIZE)
z_data = deque([0.0]*BUFFER_SIZE, maxlen=BUFFER_SIZE)

pitch_data = deque([0.0]*BUFFER_SIZE, maxlen=BUFFER_SIZE)
roll_data = deque([0.0]*BUFFER_SIZE, maxlen=BUFFER_SIZE)

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
    elif sender_uuid == UUID_PITCH:
        pitch_data.append(val)
    elif sender_uuid == UUID_ROLL:
        roll_data.append(val)

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
        await client.start_notify(UUID_PITCH, notification_handler)
        await client.start_notify(UUID_ROLL, notification_handler)
        
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
            await client.stop_notify(UUID_PITCH)
            await client.stop_notify(UUID_ROLL)
            await client.disconnect()

def start_ble_thread():
    """Lance la boucle asynchrone BLE dans un Thread séparé."""
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(run_ble())

# --- 2. Display (Matplotlib) ---
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(8, 8))
plt.subplots_adjust(hspace=0.4) 

# Graph 1 : Movement
ax1.set_title("Accélération (g)")
ax1.set_ylim(-2.5, 2.5)
ax1.set_xlim(0, BUFFER_SIZE)
line_x, = ax1.plot(x_data, label='X', color='red')
line_y, = ax1.plot(y_data, label='Y', color='green')
line_z, = ax1.plot(z_data, label='Z', color='blue')
ax1.legend(loc='upper right', fontsize='small')

# Graph 2 : Angle
ax2.set_title("Inclinaison (Degrés)")
ax2.set_ylim(-180, 180)
ax2.set_xlim(0, BUFFER_SIZE)
line_pitch, = ax2.plot(pitch_data, label='Rotation Y', color='orange', lw=2)
line_roll,  = ax2.plot(roll_data, label='Rotation X', color='purple', lw=2)
ax2.legend(loc='upper right', fontsize='small')

def update_plot(frame):
    """Mise à jour périodique des courbes avec le contenu actuel du buffer."""
    line_x.set_ydata(list(x_data))
    line_y.set_ydata(list(y_data))
    line_z.set_ydata(list(z_data))
    line_pitch.set_ydata(list(pitch_data))
    line_roll.set_ydata(list(roll_data))

    return line_x, line_y, line_z, line_pitch, line_roll

# --- 3. MAIN  ---
if __name__ == "__main__":

    # Starting the BLE in the background
    ble_thread = threading.Thread(target=start_ble_thread, daemon=True)
    ble_thread.start()

    # Starting the display window (update every 50ms)
    ani = animation.FuncAnimation(fig, update_plot, interval=50, blit=True, cache_frame_data=False)
    
    plt.show() 
    
    print("Fermeture du programme...")