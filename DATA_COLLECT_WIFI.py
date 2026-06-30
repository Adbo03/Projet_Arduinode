import requests
import os
import shutil
import threading
import time 
import subprocess
from CONVERSION_BIN_CSV import process_file 

def get_current_wifi():
    """Récupère le nom (SSID) du réseau Wi-Fi auquel le PC est actuellement connecté."""
    try:
        cmd = subprocess.check_output("netsh wlan show interfaces", shell=True, text=True, encoding="cp1252")
        for line in cmd.split("\n"):
            if "SSID" in line and "BSSID" not in line:
                return line.split(":")[1].strip()
            
    except subprocess.CalledProcessError:
        return None 
    
    except Exception as e:
        print(f"Erreur : {e}")
    return None

def configure_wifi(ssid, password):
    """ Génère un profil XML pour le Wi-Fi, l'installe sur Windows, et force la connexion vers l'Arduinode.
    """

    xml_profile = f"""<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
    <name>{ssid}</name>
    <SSIDConfig>
        <SSID>
            <name>{ssid}</name>
        </SSID>
    </SSIDConfig>
    <connectionType>ESS</connectionType>
    <connectionMode>manual</connectionMode>
    <MSM>
        <security>
            <authEncryption>
                <authentication>WPA2PSK</authentication>
                <encryption>AES</encryption>
                <useOneX>false</useOneX>
            </authEncryption>
            <sharedKey>
                <keyType>passPhrase</keyType>
                <protected>false</protected>
                <keyMaterial>{password}</keyMaterial>
            </sharedKey>
        </security>
    </MSM>
</WLANProfile>"""

    xml_filename = "wifi_temp_profile.xml"
    
    try:

        with open(xml_filename, "w", encoding="utf-8") as f:
            f.write(xml_profile)
            
        print(f"Configuration du profil Wi-Fi pour {ssid}...")
        subprocess.run(
            f'netsh wlan add profile filename="{xml_filename}"', 
            shell=True, check=True, stdout=subprocess.DEVNULL
        )
        
        os.remove(xml_filename)
        
        print(f"Connexion à {ssid}...")
        subprocess.run(
            f'netsh wlan connect name="{ssid}"', 
            shell=True, check=True, stdout=subprocess.DEVNULL
        )
        
        # Attente de l'attribution de l'IP par l'Arduinode
        for i in range(12):
            time.sleep(1)
            try:
                r = requests.get("http://192.168.4.1/list", timeout=1.5)
                if r.status_code == 200:
                    print(f"Connecté avec succès ! IP réseau obtenue.")
                    return True
            except requests.RequestException:
                continue
                
    except Exception as e:
        print(f"❌ Erreur lors de la configuration ou connexion Wi-Fi : {e}")
        if os.path.exists(xml_filename):
            os.remove(xml_filename)
            
    return False

def connect_to_wifi(ssid_cible):
    """Permet la connexion au réseau Wifi de l'arduinode."""

    try:
        subprocess.run(f'netsh wlan connect name="{ssid_cible}"', shell=True, check=True, stdout=subprocess.DEVNULL)
        
        for _ in range(10):
            time.sleep(1)
            try:
                r = requests.get("http://192.168.4.1/list", timeout=1.5)
                if r.status_code == 200:
                    print(f"Connecté avec succès à {ssid_cible} !")
                    return True
            except requests.RequestException:
                continue

    except Exception as e:
        print(f"Erreur : {e}")
    return False

def collect_data_wifi(save_mode="CSV + BIN"):
    """ Télécharge tous les fichiers de l'ESP32 par Wi-Fi, les supprime de la SD, puis applique la conversion sélectionnée dans l'IHM."""

    SSID_ARDUINODE = "Arduinode_WiFi"
    PWD_ARDUINODE = "password123"
    IP_ESP = "192.168.4.1"
    url_root = f"http://{IP_ESP}"

    temp_folder = "./DUMP_WIFI"
    csv_folder = "./DATACSV"
    raw_folder = "./DATARAW"

    # old_wifi = get_current_wifi()
    # connected_to_node = False 

    # if old_wifi == SSID_ARDUINODE:
    #     connected_to_node = True
    # else:
    connected_to_node = configure_wifi(SSID_ARDUINODE, PWD_ARDUINODE)
    
    if not connected_to_node:
        print("Erreur : impossible de basculer sur le WiFi de la carte. Opération annulée.")
    
    try:
        os.makedirs(temp_folder, exist_ok=True)
        os.makedirs(csv_folder, exist_ok=True)
        os.makedirs(raw_folder, exist_ok=True)

        # Récupération de la liste des fichiers
        print("Lecture de la carte SD via Wi-Fi...")
        response = requests.get(f"{url_root}/list", timeout=5)
        if response.status_code != 200 or not response.text.strip():
            print("Aucun fichier binaire à récupérer (Carte SD vide).")
            return

        files = response.text.strip().split("\n")
        print(f"{len(files)} fichier(s) détecté(s) sur l'Arduinode.")

        for file in files:
            file_name = file.lstrip('/')
            path_temp_bin = os.path.join(temp_folder, file_name)
            
            print(f"\n Téléchargement de : {file_name}...")
            
            with requests.get(f"{url_root}/download", params={"name": file}, stream=True) as r:
                r.raise_for_status()
                with open(path_temp_bin, 'wb') as f:
                    for chunk in r.iter_content(chunk_size=8192):
                        f.write(chunk)
            
            if os.path.exists(path_temp_bin) and os.path.getsize(path_temp_bin) > 0:
                print(f"Fichier {file_name} sauvegardé en local.")
                
                # Suppression du fichier de la carte SD
                del_resp = requests.get(f"{url_root}/delete", params={"name": file})
                if del_resp.status_code == 200:
                    print(f"Supprimé avec succès de la carte SD.")
                else:
                    print(f"Erreur de suppression sur la SD (fichier conservé sur la carte).")
                
                # Conversion du fichier
                name_without_ext = os.path.splitext(file_name)[0]
                path_csv = os.path.join(csv_folder, f"{name_without_ext}.csv")
                path_bin = os.path.join(raw_folder, file_name)
                
                print(f"Type(s) de fichiers de sauvegardé(s) : [{save_mode}]")
                
                if save_mode == "CSV":
                    process_file(path_temp_bin, path_csv)
                    os.remove(path_temp_bin)
                    
                elif save_mode == "BIN":
                    shutil.move(path_temp_bin, path_bin)
                    
                elif save_mode == "CSV + BIN":
                    process_file(path_temp_bin, path_csv)
                    shutil.move(path_temp_bin, path_bin)
            else:
                print(f"Erreur critique lors du téléchargement de {file_name}. Le fichier est probablement vide -> Traitement annulé pour ce fichier.")

        print("\nFin des opérations de téléchargement et de conversion.")
        
        # Demande de redémarrage à l'ESP32 pour qu'il rebascule automatiquement en BLE
        print("Demande de réinitialisation à l'Arduinode (Retour au mode BLE)...")
        requests.get(f"{url_root}/reboot", timeout=2)

    except Exception as e:
        print(f"Erreur lors de la collecte Wi-Fi : {e}")

    finally:
        # if connected_to_node and old_wifi and old_wifi != SSID_ARDUINODE:
        #     subprocess.run(f'netsh wlan connect name="{old_wifi}"', shell=True, stdout=subprocess.DEVNULL)
            
        #     try:
        #         if not os.listdir(temp_folder):
        #             os.rmdir(temp_folder)
        #     except Exception:
        #         pass

        if connected_to_node:
            subprocess.run("netsh wlan disconnect", shell=True, stdout=subprocess.DEVNULL)

def start_collect(save_mode="CSV + BIN"):
    """Déclenche la récupération dans un thread pour préserver l'IHM."""
    t = threading.Thread(target= collect_data_wifi, args=(save_mode,), daemon=True)
    t.start()