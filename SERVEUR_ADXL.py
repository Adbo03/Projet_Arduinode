from collections import deque
from matplotlib.widgets import RadioButtons, Button
from bleak import BleakClient, BleakScanner
from CONVERSION_BIN_CSV import process_batch
from DATA_COLLECT_WIFI import start_collect
import asyncio
import threading
import struct
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import matplotlib.animation as animation
import math 
import sys

try:
    from winrt.windows.devices.radios import Radio, RadioKind, RadioState
except ImportError:
    print("Erreur : Veuillez installer le package requis avec : pip install winrt-Windows.Devices.Radios")
    sys.exit(1)

# --- PARAMETRAGE ---
UUID_SERVICE_ARDUINODE = "19b10000-e8f2-537e-4f6c-d104768a1214"
UUID_RAWDATA = "19b10001-e8f2-537e-4f6c-d104768a1214"
UUID_MODE = "19b10002-e8f2-537e-4f6c-d104768a1214"
UUID_RANGE = "19b10003-e8f2-537e-4f6c-d104768a1214"
UUID_ADXL_FREQUENCY = "19b10004-e8f2-537e-4f6c-d104768a1214"

UUID_SERVICE_SOURCE = "18b10000-e8f2-537e-4f6c-d104768a1214"
UUID_SOURCE_MODE = "18b10001-e8f2-537e-4f6c-d104768a1214"
UUID_SOURCE_FREQUENCY = "18b10002-e8f2-537e-4f6c-d104768a1214"

PRECISION_STABILITE = 10     # +/- exprimée en °
SCALE_FACTORS = [256000.0, 128000.0, 64000.0]
RANGE = 0
ADXL_FREQUENCY = 0
SAVE_MODE = "CSV + BIN"
BLE_ENABLE = True
SOURCE_FREQUENCY = 0
WINDOW_SIZE = 4000

needs_ui_sync = False
ble_initial_state = {'mode': 0, 'range': 0, 'freq': 0, 's_freq': 0, 's_status': 0}
block_ble_writes = False
is_recording = False
is_streaming = False
ignore_ui_events = False 
generating_impulsions = False
axes_changed = False
connect_source = False
wifi_collect = False

x_data = deque([0.0]*WINDOW_SIZE, maxlen=WINDOW_SIZE)
y_data = deque([0.0]*WINDOW_SIZE, maxlen=WINDOW_SIZE)
z_data = deque([0.0]*WINDOW_SIZE, maxlen=WINDOW_SIZE)
latest_pitch = 0.0
latest_roll = 0.0

name_arduinode = ""
client_Arduinode = None
client_Source = None
ble_loop = None
ble_running = True
reconnect_delay = 5

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
            latest_pitch = math.atan2(last_y, last_z) * 180.0 / math.pi
            latest_roll = math.atan2(-last_x, math.sqrt(last_y * last_y + last_z * last_z)) * 180.0 / math.pi

        except ValueError:
            # Sécurité en cas de division par zéro instable
            pass

async def discover_by_uuid():

    if not wifi_collect:
        print("Recherche automatique des cartes...")
    
    devices_dict = await BleakScanner.discover(timeout=5.0, return_adv=True)
    
    mac_arduinode = None
    mac_source = None

    for address, (device, adv_data) in devices_dict.items():
        uuids = adv_data.service_uuids
        
        if UUID_SERVICE_ARDUINODE in uuids:
            mac_arduinode = address
            print(f"Arduinode détecté : {address}")
            
        elif UUID_SERVICE_SOURCE in uuids:
            mac_source = address
            print(f"Source détecté : {address}")

    return mac_arduinode, mac_source

async def toggle_radio(type_radio, status=True):
    """ Permet d'activer ou de desactiver le WiFI (ou le Bluetooth) """

    radios = await Radio.get_radios_async()
    
    for radio in radios:
        if radio.kind == type_radio:    
            new_state = RadioState.ON if status else RadioState.OFF
            
            if radio.state != new_state:
                await radio.set_state_async(new_state)
            
            break

async def run_ble():
    global client_Arduinode, client_Source, ble_running, btnRecord, BLE_ENABLE, wifi_collect
    global ble_initial_state, needs_ui_sync, name_arduinode

    while ble_running:

        if not BLE_ENABLE:
            await asyncio.sleep(1)
            continue
            
        MAC_ARDUINODE, MAC_SOURCE = await discover_by_uuid()
        
        if client_Arduinode is None:
            if not MAC_ARDUINODE:

                if not wifi_collect:
                    print("Aucun Arduinode à portée. Nouvelle tentative dans 5 secondes...")
                
                await asyncio.sleep(5)
                continue

            client_1 = None

        if client_Source is None:
            if connect_source:
                if not MAC_SOURCE:

                    if not wifi_collect:
                        print("Source introuvable. Nouvelle tentative dans 5 secondes...")
                    
                    await asyncio.sleep(5)
                    continue
                
            client_2 = None

        print("Tentative de connexion aux cartes trouvées...")
        
        try:
            wifi_collect = False

            if client_Arduinode is None:
                client_1 = BleakClient(MAC_ARDUINODE, disconnected_callback=on_disconnect)
                await client_1.connect()
                client_Arduinode = client_1
                name_arduinode = client_1.name
                print("Connecté à "+ name_arduinode)

            if connect_source and client_Source is None:
                client_2 = BleakClient(MAC_SOURCE)
                await client_2.connect()
                client_Source = client_2
                print("Connecté à la source !")

            # Synchronisation des paramètres
            try:
                mode_bytes = await client_1.read_gatt_char(UUID_MODE)
                range_bytes = await client_1.read_gatt_char(UUID_RANGE)
                freq_bytes = await client_1.read_gatt_char(UUID_ADXL_FREQUENCY)

                ble_initial_state = {
                    'mode': mode_bytes[0],
                    'range': range_bytes[0],
                    'freq': freq_bytes[0]
                }

                if connect_source:
                    s_mode_bytes = await client_2.read_gatt_char(UUID_SOURCE_MODE)
                    s_freq_bytes = await client_2.read_gatt_char(UUID_SOURCE_FREQUENCY)

                    ble_initial_state['source_mode'] = s_mode_bytes[0]
                    ble_initial_state['source_freq'] = s_freq_bytes[0]

                needs_ui_sync = True

            except Exception as read_error:
                print("Échec de la lecture initiale des configurations :", read_error)
            
            if BLE_ENABLE:
                print("Démarrage du flux de données...")
                await client_1.start_notify(UUID_RAWDATA, notification_handler)
            else:
                print("Connexion établie, mais communication en pause.")
            
            # Boucle de maintien de la connexion tant que les clients sont connectés
            while client_1.is_connected and ble_running and (not connect_source or (connect_source and client_2 is not None and client_2.is_connected)):
                await asyncio.sleep(1)

        except Exception as e:
            print("Erreur lors de la connexion BLE :", e)

        # Gestion des déconnexions
        try:
            if not ble_running:
                if client_1 is not None and client_1.is_connected:
                    await client_1.disconnect()

                if client_2 is not None and client_2.is_connected:
                    await client_2.disconnect()

        except Exception as disconnect_error:
            print("Erreur lors de la déconnexion forcée :", disconnect_error)
        
        if client_1 and not client_1.is_connected:
            client_Arduinode = None

        if client_2 and not client_2.is_connected:
            client_Source = None

        if ble_running:
            print(f"Un des appareils n'est pas connecté. Nouvelle tentative dans {reconnect_delay} secondes...")
            await asyncio.sleep(reconnect_delay)

    print("Arrêt complet du BLE.")

def on_disconnect(client):
    print("Déconnecté de la carte")

def change_range(label):
    global RANGE, block_ble_writes, ignore_ui_events

    if ignore_ui_events:
        return
    
    dict_modes = {"-2g/+2g": 0, "-4g/+4g": 1, "-8g/+8g": 2}
    val = dict_modes[label]

    if is_recording:
        if val != RANGE:
            old_block = block_ble_writes
            block_ble_writes = True
            radioRange.set_active(RANGE) 
            block_ble_writes = old_block
        return
    
    RANGE = val

    if client_Arduinode and not block_ble_writes:
        asyncio.run_coroutine_threadsafe(
            client_Arduinode.write_gatt_char(UUID_RANGE, bytearray([val]), response=False), 
            ble_loop
        )

def change_frequency(label):
    global WINDOW_SIZE, ADXL_FREQUENCY, x_data, y_data, z_data, time_axis, ignore_ui_events, block_ble_writes

    if ignore_ui_events:
        return
    
    dict_modes = {"4000 Hz": 0, 
                  "2000 Hz": 1, 
                  "1000 Hz": 2, 
                  "500 Hz": 3, 
                  "250 Hz": 4, 
                  "125 Hz": 5, 
                  "62.5 Hz": 6, 
                  "31.25 Hz": 7, 
                  "15.625 Hz": 8, 
                  "7.813 Hz": 9, 
                  "3.906 Hz": 10}
    
    val = dict_modes[label]

    if is_recording:
        if val != ADXL_FREQUENCY:
            old_block = block_ble_writes
            block_ble_writes = True
            radioFrequency.set_active(ADXL_FREQUENCY) 
            block_ble_writes = old_block
        return

    ADXL_FREQUENCY = val 

    if client_Arduinode and not block_ble_writes:
        asyncio.run_coroutine_threadsafe(
            client_Arduinode.write_gatt_char(UUID_ADXL_FREQUENCY, bytearray([val]), response=False), 
            ble_loop
        )

    if val >= 6:
        new_size = float(label.split()[0])
        new_size = int(new_size)
    
    else:
        new_size = int(label.split()[0])

    WINDOW_SIZE = new_size

    old_x, old_y, old_z = list(x_data), list(y_data), list(z_data)

    x_data = deque([0.0] * max(0, new_size - len(old_x)) + old_x[-new_size:], maxlen=new_size)
    y_data = deque([0.0] * max(0, new_size - len(old_y)) + old_y[-new_size:], maxlen=new_size)
    z_data = deque([0.0] * max(0, new_size - len(old_z)) + old_z[-new_size:], maxlen=new_size)

    time_axis = [i/WINDOW_SIZE for i in range(WINDOW_SIZE)]

def change_save(label):
    global SAVE_MODE
    SAVE_MODE = label

def change_ble_state(label):
    global BLE_ENABLE
    BLE_ENABLE = (label == 'Activer')

    if client_Arduinode and client_Arduinode.is_connected:
        if BLE_ENABLE:
            print("\nReprise de la communication (Lecture)...\n")
            asyncio.run_coroutine_threadsafe(
                client_Arduinode.start_notify(UUID_RAWDATA, notification_handler), 
                ble_loop
            )
        else:
            print("\nCommunication en pause (Stop)...\n")
            asyncio.run_coroutine_threadsafe(
                client_Arduinode.stop_notify(UUID_RAWDATA), 
                ble_loop
            )

    else:
        if BLE_ENABLE:
            print("\nRecherche Bluetooth relancée...\n")
        else:
            print("\nBluetooth désactivé. Déconnexion en cours...\n")

def change_source_freq(label):
    global SOURCE_FREQUENCY, block_ble_writes, ignore_ui_events

    if ignore_ui_events:
        return
    
    dict_modes = {"10 Hz": 0, 
                  "20 Hz": 1, 
                  "30 Hz": 2, 
                  "40 Hz": 3, 
                  "50 Hz": 4, 
                  "60 Hz": 5, 
                  "70 Hz": 6, 
                  "80 Hz": 7, 
                  "90 Hz": 8, 
                  "100 Hz": 9,
                  "150 Hz": 10,
                  "200 Hz": 11,
                  "250 Hz": 12}
    
    val = dict_modes[label]

    if generating_impulsions:
        if val != SOURCE_FREQUENCY:
            old_block = block_ble_writes
            block_ble_writes = True
            radioSourcefreq.set_active(SOURCE_FREQUENCY) 
            block_ble_writes = old_block
        return
    
    SOURCE_FREQUENCY = val

    if client_Source and not block_ble_writes:
        asyncio.run_coroutine_threadsafe(
            client_Source.write_gatt_char(UUID_SOURCE_FREQUENCY, bytearray([val]), response=False), 
            ble_loop
        )

def toggle_recording(event):
    global btnRecord, btnStream, is_recording, is_streaming

    if not is_recording:
        is_recording = True

        raxRange.set_facecolor("#9d9d9d")
        raxFrequency.set_facecolor("#9d9d9d")

        btnRecord.label.set_text("Arrêter acquisition")
        btnRecord.color = "#e22200"
        btnRecord.hovercolor = "#d22200"

        # Reinitialisation du bouton si le mode 'STREAM' etait activé
        if is_streaming:
            is_streaming = False

            btnStream.label.set_text("Lancer stream")
            btnStream.color = "#00aeff"
            btnStream.hovercolor = "#0299de"

        if client_Arduinode and not block_ble_writes:
            asyncio.run_coroutine_threadsafe(
                client_Arduinode.write_gatt_char(UUID_MODE, bytearray([2]), response=False), 
                ble_loop
            )

    else:
        is_recording = False
    
        raxRange.set_facecolor("#1934e278")
        raxFrequency.set_facecolor("#1934e278")

        btnRecord.label.set_text("Lancer acquisition")
        btnRecord.color = "#ffb300"
        btnRecord.hovercolor = "#df9e05"

        if client_Arduinode and not block_ble_writes:
            asyncio.run_coroutine_threadsafe(
                client_Arduinode.write_gatt_char(UUID_MODE, bytearray([0]), response=False), 
                ble_loop
            )

    fig.canvas.draw_idle() 

def toggle_stream(event):
    global btnStream, btnRecord, is_streaming, is_recording

    if not is_streaming:
        is_streaming = True

        btnStream.label.set_text("Arrêter stream")
        btnStream.color = "#e22200"
        btnStream.hovercolor = "#d22200"

        # Reinitialisation du bouton si le mode "ENREGISTREMENT" etait activé
        if is_recording:
            is_recording = False
    
            raxRange.set_facecolor("#1934e278")
            raxFrequency.set_facecolor("#1934e278")

            btnRecord.label.set_text("Lancer acquisition")
            btnRecord.color = "#ffb300"
            btnRecord.hovercolor = "#df9e05"

        if client_Arduinode and not block_ble_writes:
            asyncio.run_coroutine_threadsafe(
                client_Arduinode.write_gatt_char(UUID_MODE, bytearray([1]), response=False), 
                ble_loop
            )

    else:
        is_streaming = False

        btnStream.label.set_text("Lancer stream")
        btnStream.color = "#00aeff"
        btnStream.hovercolor = "#0299de"

        if client_Arduinode and not block_ble_writes:
            asyncio.run_coroutine_threadsafe(
                client_Arduinode.write_gatt_char(UUID_MODE, bytearray([0]), response=False), 
                ble_loop
            )

    fig.canvas.draw_idle() 

def toggle_source(event):
    global btnSource, generating_impulsions

    if not connect_source:
        return
    
    if not generating_impulsions:
        generating_impulsions = True

        raxSourcefreq.set_facecolor("#9d9d9d")

        btnSource.label.set_text("Désactiver la source")
        btnSource.color = "#e22200"
        btnSource.hovercolor = "#d22200"

        if client_Source and not block_ble_writes:
            asyncio.run_coroutine_threadsafe(
                client_Source.write_gatt_char(UUID_SOURCE_MODE, bytearray([1]), response=False), 
                ble_loop
            )

    else:
        generating_impulsions = False
    
        raxSourcefreq.set_facecolor("#1934e278")

        btnSource.label.set_text("Activer la source")
        btnSource.color = "#26ff00"
        btnSource.hovercolor = "#12d900"

        if client_Source and not block_ble_writes:
            asyncio.run_coroutine_threadsafe(
                client_Source.write_gatt_char(UUID_SOURCE_MODE, bytearray([0]), response=False), 
                ble_loop
            )

    fig.canvas.draw_idle() 

def toggle_connect_src(label):
    global connect_source

    if label == "Activer":
        connect_source = True

    else:
        connect_source = False

def launch_extraction(event):
    process_batch(SAVE_MODE)

def launch_wifi_collect(event):
    global wifi_collect

    if client_Arduinode and not block_ble_writes:
        asyncio.run_coroutine_threadsafe(
            client_Arduinode.write_gatt_char(UUID_MODE, bytearray([3]), response=False), 
            ble_loop
        )
        wifi_collect = True
        start_collect(name_arduinode+"_WIFI")

def on_limits_changed(ax):
    global axes_changed
    axes_changed = True

# --- Affichage (Matplotlib) ---
fig, ax1 = plt.subplots(figsize=(10, 6))
plt.subplots_adjust(left=0.25, right=0.75, bottom = 0.25)

# Boutons  
raxBLE = plt.axes([0.02, 0.79, 0.15, 0.12], facecolor="#1934e278", title="Recherche Bluetooth")
radioBLE = RadioButtons(raxBLE, ('Activer', 'Désactiver'))
radioBLE.on_clicked(change_ble_state)

raxRange = plt.axes([0.02, 0.60, 0.15, 0.12], facecolor = "#1934e278", title="Plage de mesure")
radioRange = RadioButtons(raxRange, ('-2g/+2g', '-4g/+4g', '-8g/+8g'))
radioRange.on_clicked(change_range)

raxFrequency = plt.axes([0.02, 0.27, 0.15, 0.26], facecolor = "#1934e278", title="Echantillonnage")
radioFrequency = RadioButtons(raxFrequency, ('4000 Hz', '2000 Hz', '1000 Hz', '500 Hz', '250 Hz', '125 Hz', '62.5 Hz', '31.25 Hz', '15.625 Hz', '7.813 Hz', '3.906 Hz'))
radioFrequency.on_clicked(change_frequency)

raxSave = plt.axes([0.02, 0.08, 0.15, 0.12], facecolor = "#1934e278", title="Sauvegarde PC")
radioSave = RadioButtons(raxSave, ('CSV', 'BIN', 'CSV + BIN'))
radioSave.on_clicked(change_save)
radioSave.set_active(2)

raxConnect_src = plt.axes([0.80, 0.79, 0.15, 0.12], facecolor="#1934e278", title="Connexion Source")
radioConnect_src = RadioButtons(raxConnect_src, ('Activer', 'Désactiver'))
radioConnect_src.on_clicked(toggle_connect_src)
radioConnect_src.set_active(1)

raxSourcefreq = plt.axes([0.80, 0.41, 0.15, 0.31], facecolor = "#1934e278", title="Frequence ricker (+/-2.5Hz)")
radioSourcefreq = RadioButtons(raxSourcefreq, ('10 Hz', '20 Hz', '30 Hz', '40 Hz', '50 Hz', '60 Hz', '70 Hz', '80 Hz', '90 Hz', '100 Hz', '150 Hz', '200 Hz', '250 Hz'))
radioSourcefreq.on_clicked(change_source_freq)

axSource = plt.axes([0.80, 0.31, 0.15, 0.05])
btnSource = Button(axSource, 'Activer source')
btnSource.on_clicked(toggle_source)
btnSource.color = "#9d9d9d"
btnSource.hovercolor = "#9d9d9d"

axRecord = plt.axes([0.80, 0.25, 0.15, 0.05])
btnRecord = Button(axRecord, 'Lancer acquisition')
btnRecord.on_clicked(toggle_recording)
btnRecord.color = "#ffb300"
btnRecord.hovercolor = "#df9e05"

axStream = plt.axes([0.80, 0.19, 0.15, 0.05])
btnStream = Button(axStream, 'Lancer stream')
btnStream.on_clicked(toggle_stream)
btnStream.color = "#00aeff"
btnStream.hovercolor = "#0299de"

axExtract = plt.axes([0.80, 0.13, 0.15, 0.05])
btnExtract = Button(axExtract, 'Extraire sur PC')
btnExtract.on_clicked(launch_extraction)
btnExtract.color = "#ffff07"
btnExtract.hovercolor = "#d5d502"

axExtractWifi = plt.axes([0.80, 0.07, 0.15, 0.05])
btnExtractWifi = Button(axExtractWifi, 'Collecte WiFi')
btnExtractWifi.on_clicked(launch_wifi_collect)
btnExtractWifi.color = "#c907ff"
btnExtractWifi.hovercolor = "#a706d3"

# Graphe des accélérations 
ax1.set_title("Accélération (g)")
ax1.set_xlabel("Temps (s)")
ax1.set_ylim(-8.5, 8.5)   
ax1.yaxis.set_major_locator(ticker.MultipleLocator(1)) 
time_axis = [i/WINDOW_SIZE for i in range(WINDOW_SIZE)]
line_x, = ax1.plot(time_axis, x_data, label='X', color='red')
line_y, = ax1.plot(time_axis, y_data, label='Y', color='green')
line_z, = ax1.plot(time_axis, z_data, label='Z', color='blue')
ax1.legend(loc='upper right', fontsize='small')
ax1.callbacks.connect('xlim_changed', on_limits_changed)
ax1.callbacks.connect('ylim_changed', on_limits_changed)

props = dict(boxstyle='round', facecolor="#fefefe")

# Angles de rotation
ax2 = fig.add_axes([0.25, 0.05, 0.5, 0.15])
ax2.axis('off')

text_pitch = ax2.text(0.525, 0.5, "Inclinaison Y : 0.00°", fontsize=14, verticalalignment='top', bbox=props, transform=ax2.transAxes)
text_roll  = ax2.text(0.021, 0.5, "Inclinaison X : 0.00°", fontsize=14, verticalalignment='top', bbox=props, transform=ax2.transAxes)

def update_plot(frame):
    """Mise à jour périodique des courbes avec le contenu actuel du buffer."""
    global latest_pitch, latest_roll, needs_ui_sync, block_ble_writes, is_recording, is_streaming, generating_impulsions
    global axes_changed

    if axes_changed:
        axes_changed = False
        fig.canvas.draw()
        
    if needs_ui_sync:
        needs_ui_sync = False
        block_ble_writes = True  # Bloque l'envoi d'ordres BLE pendant la mise à jour de l'IHM
        try:
            fig.suptitle(name_arduinode, fontsize=16, fontweight='bold')

            radioRange.set_active(ble_initial_state['range'])
            radioFrequency.set_active(ble_initial_state['freq'])

            if connect_source:
                radioSourcefreq.set_active(ble_initial_state["source_freq"])

            if ble_initial_state["mode"] == 1:
                is_streaming = True
                is_recording = False

                btnStream.label.set_text("Arrêter stream")
                btnStream.color = "#e22200"
                btnStream.hovercolor = "#d22200"

                btnRecord.label.set_text("Lancer acquisition")
                btnRecord.color = "#ffb300"
                btnRecord.hovercolor = "#df9e05"

            elif ble_initial_state["mode"] == 2:
                is_recording = True
                is_streaming = False

                raxRange.set_facecolor("#9d9d9d")
                raxFrequency.set_facecolor("#9d9d9d")

                btnRecord.label.set_text("Arrêter acquisition")
                btnRecord.color = "#e22200"
                btnRecord.hovercolor = "#d22200"

                btnStream.label.set_text("Lancer stream")
                btnStream.color = "#00aeff"
                btnStream.hovercolor = "#0299de"
            
            else:
                is_recording = False
                is_streaming = False

                raxRange.set_facecolor("#1934e278")
                raxFrequency.set_facecolor("#1934e278")

                btnRecord.label.set_text("Lancer acquisition")
                btnRecord.color = "#ffb300"
                btnRecord.hovercolor = "#df9e05"

                btnStream.label.set_text("Lancer stream")
                btnStream.color = "#00aeff"
                btnStream.hovercolor = "#0299de"

            if connect_source:
                
                if ble_initial_state["source_mode"] == 1:
                    generating_impulsions = True
                    raxSourcefreq.set_facecolor("#9d9d9d")

                    btnSource.label.set_text("Désactiver la source")
                    btnSource.color = "#e22200"
                    btnSource.hovercolor = "#d22200"
                
                else:
                    generating_impulsions = False

                    raxSourcefreq.set_facecolor("#1934e278")

                    btnSource.label.set_text("Activer la source")
                    btnSource.color = "#26ff00"
                    btnSource.hovercolor = "#12d900"

            fig.canvas.draw_idle()
            print("IHM synchronisée avec le statut de la carte !")

        except Exception as e:
            print("Erreur lors de la synchronisation visuelle :", e)

        block_ble_writes = False
        needs_ui_sync = False

    line_x.set_data(time_axis, list(x_data.copy()))
    line_y.set_data(time_axis, list(y_data.copy()))
    line_z.set_data(time_axis, list(z_data.copy()))
    text_pitch.set_text(f"Inclinaison Y : {latest_pitch:>.2f}°")
    text_roll.set_text(f"Inclinaison X : {latest_roll:>.2f}°")

    if abs(latest_pitch) < PRECISION_STABILITE :
        text_pitch.get_bbox_patch().set_facecolor("#c3e6cb") 
        text_roll.get_bbox_patch().set_edgecolor("#155724")  
        text_pitch.set_color("#155724")                     
    else:
        text_pitch.get_bbox_patch().set_facecolor("#f5c6cb") 
        text_roll.get_bbox_patch().set_edgecolor("#721c24")
        text_pitch.set_color("#721c24")                    

    
    if abs(latest_roll) < PRECISION_STABILITE :
        text_roll.get_bbox_patch().set_facecolor("#c3e6cb")
        text_roll.get_bbox_patch().set_edgecolor("#155724")  
        text_roll.set_color("#155724")                      
    else:
        text_roll.get_bbox_patch().set_facecolor("#f5c6cb") 
        text_roll.get_bbox_patch().set_edgecolor("#721c24")
        text_roll.set_color("#721c24")

    return line_x, line_y, line_z, text_pitch, text_roll

# --- MAIN  ---
if __name__ == "__main__":

    # Lancement du BLE en arrière-plan 
    ble_loop = asyncio.new_event_loop()
    
    def start_ble():
        asyncio.set_event_loop(ble_loop)

        ble_loop.run_until_complete(toggle_radio(RadioKind.BLUETOOTH))
        ble_loop.run_until_complete(toggle_radio(RadioKind.WI_FI))

        ble_loop.run_until_complete(run_ble())

    threading.Thread(target=start_ble, daemon=True).start()
    
    ani = animation.FuncAnimation(fig, update_plot, interval=10, blit=True, cache_frame_data=False)

    def on_close(event):
        global ble_running
        ble_running = False
        print("Fermeture du programme...")

    fig.canvas.mpl_connect('close_event', on_close)
    
    plt.show()
    
    