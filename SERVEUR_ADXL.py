from collections import deque
from matplotlib.widgets import RadioButtons, Button
from bleak import BleakClient, BleakScanner
from CONVERSION_BIN_CSV import process_batch
import asyncio
import threading
import struct
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import matplotlib.animation as animation
import math 

# --- PARAMETRAGE ---
DEVICE_NAME = "ADXL355Z" 
UUID_RAWDATA = "19b10001-e8f2-537e-4f6c-d104768a1214"
UUID_MODE = "19b10002-e8f2-537e-4f6c-d104768a1214"
UUID_RANGE = "19b10003-e8f2-537e-4f6c-d104768a1214"
UUID_FREQUENCY = "19b10004-e8f2-537e-4f6c-d104768a1214"

PRECISION_STABILITE = 1     # +/- exprimée en °
SCALE_FACTORS = [256000.0, 128000.0, 64000.0]
RANGE = 0
WINDOW_SIZE = 4000
SAVE_MODE = "CSV"
BLE_ENABLE = True

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
        last_x, last_y, last_z = (0,0,0)
        num_samples = len(data) // sample_size

        for i in range(num_samples):
            offset = i * sample_size
            
            x_raw, y_raw, z_raw = struct.unpack_from('<iii', data, offset)
            
            ax = x_raw / SCALE_FACTORS[RANGE]
            ay = y_raw / SCALE_FACTORS[RANGE]
            az = z_raw / SCALE_FACTORS[RANGE]
            
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
    global client_global, ble_running, BLE_ENABLE

    while ble_running:

        if not BLE_ENABLE:
            await asyncio.sleep(1)
            continue

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

            print("Connecté au BLE !")
            
            if BLE_ENABLE:
                print("Démarrage du flux de données...")
                await client.start_notify(UUID_RAWDATA, notification_handler)
            else:
                print("Connexion établie, mais communication en pause.")
            
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

def change_range(label):
    global RANGE

    dict_modes = {"2g": 0, "4g": 1, "8g": 2}
    val = dict_modes[label]
    RANGE = val

    if client_global:
        asyncio.run_coroutine_threadsafe(
            client_global.write_gatt_char(UUID_RANGE, bytearray([val]), response=False), 
            ble_loop
        )

def change_frequency(label):
    global WINDOW_SIZE, x_data, y_data, z_data

    dict_modes = {"4000 Hz": 0, "2000 Hz": 1, "1000 Hz": 2, "500 Hz": 3}
    val = dict_modes[label]

    if client_global:
        asyncio.run_coroutine_threadsafe(
            client_global.write_gatt_char(UUID_FREQUENCY, bytearray([val]), response=False), 
            ble_loop
        )

    new_size = int(label.split()[0])
    WINDOW_SIZE = new_size

    old_x, old_y, old_z = list(x_data), list(y_data), list(z_data)

    x_data = deque([0.0] * max(0, new_size - len(old_x)) + old_x[-new_size:], maxlen=new_size)
    y_data = deque([0.0] * max(0, new_size - len(old_y)) + old_y[-new_size:], maxlen=new_size)
    z_data = deque([0.0] * max(0, new_size - len(old_z)) + old_z[-new_size:], maxlen=new_size)

    line_x.set_visible(False)
    line_y.set_visible(False)
    line_z.set_visible(False)

    ax1.set_xlim(0, WINDOW_SIZE)
    fig.canvas.draw()

    line_x.set_visible(True)
    line_y.set_visible(True)
    line_z.set_visible(True)

def change_save(label):
    global SAVE_MODE
    SAVE_MODE = label

def change_ble_state(label):
    global BLE_ENABLE
    BLE_ENABLE = (label == 'Activer')

    if client_global and client_global.is_connected:
        if BLE_ENABLE:
            print("\nReprise de la communication (Lecture)...\n")
            asyncio.run_coroutine_threadsafe(
                client_global.start_notify(UUID_RAWDATA, notification_handler), 
                ble_loop
            )
        else:
            print("\nCommunication en pause (Stop)...\n")
            asyncio.run_coroutine_threadsafe(
                client_global.stop_notify(UUID_RAWDATA), 
                ble_loop
            )

    else:
        if BLE_ENABLE:
            print("\nRecherche Bluetooth relancée...\n")
        else:
            print("\nBluetooth désactivé. Déconnexion en cours...\n")

def launch_extraction(event):
    process_batch(SAVE_MODE)

# --- Affichage (Matplotlib) ---
fig, ax1 = plt.subplots(figsize=(10, 6))
plt.subplots_adjust(left=0.25, right=0.75, bottom = 0.25) 

# Boutons  
raxBLE = plt.axes([0.83, 0.82, 0.15, 0.12], facecolor="#babebe", title="Bluetooth")
radioBLE = RadioButtons(raxBLE, ('Activer', 'Désactiver'))
radioBLE.on_clicked(change_ble_state)

raxMode = plt.axes([0.02, 0.82, 0.15, 0.12], facecolor = "#babebe", title="Modes")
radioMode = RadioButtons(raxMode, ('Temps Réel', 'Enregistrer SD'))
radioMode.on_clicked(change_mode)

raxRange = plt.axes([0.02, 0.63, 0.15, 0.12], facecolor = "#babebe", title="Interval")
radioRange = RadioButtons(raxRange, ('2g', '4g', '8g'))
radioRange.on_clicked(change_range)

raxFrequency = plt.axes([0.02, 0.44, 0.15, 0.12], facecolor = "#babebe", title="Frequence")
radioFrequency = RadioButtons(raxFrequency, ('4000 Hz', '2000 Hz', '1000 Hz', '500 Hz'))
radioFrequency.on_clicked(change_frequency)

raxSave = plt.axes([0.02, 0.25, 0.15, 0.12], facecolor = "#babebe", title="Sauvegarde PC")
radioSave = RadioButtons(raxSave, ('CSV', 'BIN', 'CSV + BIN'))
radioSave.on_clicked(change_save)

axExtract = plt.axes([0.02, 0.05, 0.15, 0.05], facecolor = "#babebe")
btnExtract = Button(axExtract, 'Extraire SD')
btnExtract.on_clicked(launch_extraction)

# Graphe des accélérations 
ax1.set_title("Accélération (g)")
ax1.set_xlabel("Echantillons")
ax1.set_ylim(-8.5, 8.5)   
ax1.yaxis.set_major_locator(ticker.MultipleLocator(1)) 
ax1.set_xlim(0, WINDOW_SIZE)
line_x, = ax1.plot(x_data, label='X', color='red')
line_y, = ax1.plot(y_data, label='Y', color='green')
line_z, = ax1.plot(z_data, label='Z', color='blue')
ax1.legend(loc='upper right', fontsize='small')

props = dict(boxstyle='round', facecolor="#fefefe")

# Angles de rotation
ax2 = fig.add_axes([0.25, 0.05, 0.5, 0.15])
ax2.axis('off')

text_pitch = ax2.text(0.6, 0.5, "Rotation Y : 0.00°", fontsize=14, verticalalignment='top', bbox=props, transform=ax2.transAxes)
text_roll  = ax2.text(0.1, 0.5, "Rotation X : 0.00°", fontsize=14, verticalalignment='top', bbox=props, transform=ax2.transAxes)

def update_plot(frame):
    """Mise à jour périodique des courbes avec le contenu actuel du buffer."""
    global latest_pitch, latest_roll

    x_axis = range(WINDOW_SIZE)
    line_x.set_data(x_axis, list(x_data.copy()))
    line_y.set_data(x_axis, list(y_data.copy()))
    line_z.set_data(x_axis, list(z_data.copy()))
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
    
    