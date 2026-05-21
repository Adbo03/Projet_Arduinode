import asyncio
import threading
import struct
from collections import deque
import matplotlib.pyplot as plt
from matplotlib.widgets import RadioButtons
import matplotlib.animation as animation
from bleak import BleakClient, BleakScanner
import math 

# --- PARAMETRAGE ---
DEVICE_NAME = "ADXL355Z" 
UUID_RAWDATA = "19b10001-e8f2-537e-4f6c-d104768a1214"
UUID_MODE = "19b10002-e8f2-537e-4f6c-d104768a1214"

# Facteur d'échelle pour l'ADXL355 (+/- 2g) : 256 000 LSB/g
SCALE_FACTOR = 256000.0

PRECISION_STABILITE = 1     # +/- exprimée en °

WINDOW_SIZE = 2000
x_data = deque([0.0]*WINDOW_SIZE, maxlen=WINDOW_SIZE)
y_data = deque([0.0]*WINDOW_SIZE, maxlen=WINDOW_SIZE)
z_data = deque([0.0]*WINDOW_SIZE, maxlen=WINDOW_SIZE)
latest_pitch = 0.0
latest_roll = 0.0

client_global = None
ble_loop = None
ble_running = True
reconnect_delay = 5

# --- BLE ---
def notification_handler(sender, data):
    """Décode les nouvelles données reçues et les stocke dans les buffer"""
    global latest_pitch, latest_roll

    sender_uuid = sender.uuid.lower()

    if sender_uuid == UUID_RAWDATA:
        sample_size = 12
        num_samples = len(data) // sample_size

        for i in range(num_samples):
            offset = i * sample_size
            
            x_raw, y_raw, z_raw = struct.unpack_from('<iii', data, offset)
            
            ax = x_raw / SCALE_FACTOR
            ay = y_raw / SCALE_FACTOR
            az = z_raw / SCALE_FACTOR
            
            x_data.append(ax)
            y_data.append(ay)
            z_data.append(az)

            # On mémorise le dernier point du paquet pour actualiser les rotations X et Y
            if i == num_samples - 1:
                last_x, last_y, last_z = ax, ay, az
        
        try:
            latest_roll = math.atan2(last_y, last_z) * 180.0 / math.pi
            latest_pitch = math.atan2(-last_x, math.sqrt(last_y * last_y + last_z * last_z)) * 180.0 / math.pi

        except ValueError:
            # Sécurité en cas de division par zéro instable
            pass

async def run_ble():
    global client_global, ble_running

    while ble_running:
        print(f"Recherche de la carte '{DEVICE_NAME}'...")
        device = await BleakScanner.find_device_by_name(DEVICE_NAME)

        if not device:
            print("Carte introuvable. Nouvelle tentative dans 5 secondes...")
            await asyncio.sleep(5)
            continue
        
        client = None

        try:
            client = BleakClient(device, disconnected_callback=on_disconnect)
            await client.connect()
            client_global = client

            print("Connecté au BLE ! Démarrage du flux de données...")

            try:
                await client.read_gatt_char(UUID_RAWDATA)
            except Exception as mtu_error:
                print(f"Avertissement MTU : {mtu_error}")

            await client.start_notify(UUID_RAWDATA, notification_handler)

            while client.is_connected and ble_running:
                await asyncio.sleep(1)

        except Exception as e:
            print("Erreur BLE :", e)

        try:
            if client is not None :
                if client.is_connected:
                    await client.disconnect()
        
        except Exception as disconnect_error:
            print("Erreur lors de la déconnexion forcée :", disconnect_error)
            
        client_global = None

        if ble_running:
            await asyncio.sleep(reconnect_delay)

    print("Arrêt complet du BLE.")

def on_disconnect(client):
    print("Déconnecté de la carte")

def change_mode(label):
    dict_modes = {"Temps Réel": 0, "Enregistrer SD": 1}
    val = dict_modes[label]
        
    if client_global:
        asyncio.run_coroutine_threadsafe(
            client_global.write_gatt_char(UUID_MODE, bytearray([val]), response=False), 
            ble_loop
        )
    print(f"Mode : {label}")

# --- Affichage (Matplotlib) ---
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 6))
plt.subplots_adjust(left=0.25, hspace=0.4) 

# Boutons de mode 
rax = plt.axes([0.02, 0.7, 0.15, 0.15], facecolor = '#f0f0f0')
radio = RadioButtons(rax, ('Temps Réel', 'Enregistrer SD'))
radio.on_clicked(change_mode)

# Graphe des accélérations 
ax1.set_title("Accélération (g)")
ax1.set_ylim(-2.5, 2.5)
ax1.set_xlim(0, WINDOW_SIZE)
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
    global latest_pitch, latest_roll, activebuf, todisplay

    line_x.set_ydata(list(x_data))
    line_y.set_ydata(list(y_data))
    line_z.set_ydata(list(z_data))
    text_pitch.set_text(f"Rotation Y : {latest_pitch:>.2f}°")
    text_roll.set_text(f"Rotation X : {latest_roll:>.2f}°")
    
    if abs(latest_pitch) < PRECISION_STABILITE :
        text_pitch.set_color("green")
    else:
        text_pitch.set_color("red")

    if abs(latest_roll) < PRECISION_STABILITE :
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
    
    ani = animation.FuncAnimation(fig, update_plot, interval=10, blit=True, cache_frame_data=False)

    def on_close(event):
        global ble_running
        ble_running = False
        print("Fermeture du programme...")

    fig.canvas.mpl_connect('close_event', on_close)
    
    plt.show()
    
    