import asyncio
import threading
import struct
from collections import deque
import matplotlib.pyplot as plt
from matplotlib.widgets import RadioButtons
import matplotlib.animation as animation
from bleak import BleakClient, BleakScanner

# --- PARAMETRAGE ---
DEVICE_NAME = "ADXL355Z" 
UUID_X = "00002a58-0000-1000-8000-00805f9b34fb"
UUID_Y = "00002a59-0000-1000-8000-00805f9b34fb"
UUID_Z = "00002a5a-0000-1000-8000-00805f9b34fb"
UUID_PITCH = "00002a5b-0000-1000-8000-00805f9b34fb"
UUID_ROLL = "00002a5c-0000-1000-8000-00805f9b34fb"
UUID_MODE = "00002a5d-0000-1000-8000-00805f9b34fb"

BUFFER_SIZE = 50
x_data = deque([0.0]*BUFFER_SIZE, maxlen=BUFFER_SIZE)
y_data = deque([0.0]*BUFFER_SIZE, maxlen=BUFFER_SIZE)
z_data = deque([0.0]*BUFFER_SIZE, maxlen=BUFFER_SIZE)
latest_pitch = 0.0
latest_roll = 0.0

client_global = None
ble_loop = None

# --- BLE ---
def notification_handler(sender, data):
    """Reçoit les données BLE et met à jour les buffers en RAM directement."""
    global latest_pitch, latest_roll

    val = struct.unpack('<f', data)[0]
    sender_uuid = sender.uuid.lower()
    
    if sender_uuid == UUID_X:
        x_data.append(val)
    elif sender_uuid == UUID_Y:
        y_data.append(val)
    elif sender_uuid == UUID_Z:
        z_data.append(val)
    elif sender_uuid == UUID_PITCH:
        latest_pitch = val
    elif sender_uuid == UUID_ROLL:
        latest_roll = val

async def run_ble():
    print(f"Recherche de la carte '{DEVICE_NAME}'...")
    global client_global
    device = await BleakScanner.find_device_by_name(DEVICE_NAME)

    if not device:
        print("Carte introuvable. Fermez la fenêtre graphique pour quitter.")
        return

    async with BleakClient(device) as client:
        print("Connecté au BLE ! Démarrage du flux de données...")
        client_global = client
        await client.start_notify(UUID_X, notification_handler)
        await client.start_notify(UUID_Y, notification_handler)
        await client.start_notify(UUID_Z, notification_handler)
        await client.start_notify(UUID_PITCH, notification_handler)
        await client.start_notify(UUID_ROLL, notification_handler)
        
        # Boucle infinie pour maintenir la connexion
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

def clear_buffers():
    """Vide les deques pour repartir de zéro sur le graphe."""
    x_data.clear(); y_data.clear(); z_data.clear()

    for _ in range(BUFFER_SIZE):
        x_data.append(0.0); y_data.append(0.0); z_data.append(0.0)

def change_mode(label):
    dict_modes = {"Temps Réel": 0, "Enregistrer SD": 1}
    val = dict_modes[label]
        
    if client_global:
        asyncio.run_coroutine_threadsafe(
            client_global.write_gatt_char(UUID_MODE, bytearray([val])), 
            ble_loop
        )
    print(f"Mode : {label}")

# --- Affichage (Matplotlib) ---
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 6))
plt.subplots_adjust(left=0.25, hspace=0.4) 

# Boutons de mode 
rax = plt.axes([0.02, 0.7, 0.15, 0.15], facecolor='#f0f0f0')
radio = RadioButtons(rax, ('Temps Réel', 'Enregistrer SD'))
radio.on_clicked(change_mode)

# Graphe des accélérations 
ax1.set_title("Accélération (g)")
ax1.set_ylim(-2.5, 2.5)
ax1.set_xlim(0, BUFFER_SIZE)
line_x, = ax1.plot(x_data, label='X', color='red')
line_y, = ax1.plot(y_data, label='Y', color='green')
line_z, = ax1.plot(z_data, label='Z', color='blue')
ax1.legend(loc='upper right', fontsize='small')

# Angles de rotation
ax2.set_axis_off()
text_pitch = ax2.text(0.7, 0.8, "Rotation Y : 0.00°", fontsize=15, ha='center', va='center', color='black', weight='regular')
text_roll  = ax2.text(0.3, 0.8, "Rotation X : 0.00°",  fontsize=15, ha='center', va='center', color='black', weight='regular')

def update_plot(frame):
    """Mise à jour périodique des courbes avec le contenu actuel du buffer."""
    line_x.set_ydata(list(x_data))
    line_y.set_ydata(list(y_data))
    line_z.set_ydata(list(z_data))
    text_pitch.set_text(f"Rotation Y : {latest_pitch:>.2f}°")
    text_roll.set_text(f"Rotation X : {latest_roll:>.2f}°")
    
    if abs(latest_pitch) < 1 :
        text_pitch.set_color("green")
    else:
        text_pitch.set_color("red")

    if abs(latest_roll) < 1 :
        text_roll.set_color("green")
    else:
        text_roll.set_color("red")

    return line_x, line_y, line_z, text_pitch, text_roll

# --- MAIN  ---
if __name__ == "__main__":

    # Lancement du BLE en arrière-plan 
    ble_loop = asyncio.new_event_loop()
    
    def start_ble():
        asyncio.set_event_loop(ble_loop)
        ble_loop.run_until_complete(run_ble())

    threading.Thread(target=start_ble, daemon=True).start()
    
    ani = animation.FuncAnimation(fig, update_plot, interval=50, blit=True, cache_frame_data=False)
    plt.show()
    
    print("Fermeture du programme...")