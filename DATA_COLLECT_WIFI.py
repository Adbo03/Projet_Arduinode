import requests
import os
import shutil
import threading
import time 
import subprocess
import re
import ctypes
from ctypes import wintypes
from CONVERSION_BIN_CSV import process_file 

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
                r = requests.get("http://192.168.4.1/download", timeout=1.5)
                if r.status_code == 200:
                    print(f"Connecté avec succès ! IP réseau obtenue.")
                    return True
            except requests.RequestException:
                continue
                
    except Exception as e:
        print(f"Erreur lors de la configuration ou connexion Wi-Fi : {e}")
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

def force_wifi_scan():
    """ Force la carte Wi-Fi à lancer un scan actif des réseaux environnants via l'API WlanAPI. """
    try:
        wlanapi = ctypes.windll.wlanapi

        class GUID(ctypes.Structure):
            _fields_ = [
                ("Data1", wintypes.DWORD),
                ("Data2", wintypes.WORD),
                ("Data3", wintypes.WORD),
                ("Data4", ctypes.c_byte * 8)
            ]

        class WLAN_INTERFACE_INFO(ctypes.Structure):
            _fields_ = [
                ("InterfaceGuid", GUID),
                ("strInterfaceDescription", ctypes.c_wchar * 256),
                ("isState", ctypes.c_int)
            ]

        class WLAN_INTERFACE_INFO_LIST(ctypes.Structure):
            _fields_ = [
                ("NumberOfItems", wintypes.DWORD),
                ("Index", wintypes.DWORD),
                ("InterfaceInfo", WLAN_INTERFACE_INFO * 1)
            ]

        handle = wintypes.HANDLE()
        negotiated_version = wintypes.DWORD()

        # Ouverture de la session WLAN
        if wlanapi.WlanOpenHandle(2, None, ctypes.byref(negotiated_version), ctypes.byref(handle)) == 0:
            p_if_list = ctypes.POINTER(WLAN_INTERFACE_INFO_LIST)()
            
            # Récupération des cartes Wi-Fi disponibles
            if wlanapi.WlanEnumInterfaces(handle, None, ctypes.byref(p_if_list)) == 0:
                if p_if_list.contents.NumberOfItems > 0:
                    # Envoi de l'ordre de scan à la première carte Wi-Fi
                    if_guid = p_if_list.contents.InterfaceInfo[0].InterfaceGuid
                    wlanapi.WlanScan(handle, ctypes.byref(if_guid), None, None, None)
                
                wlanapi.WlanFreeMemory(p_if_list)
            
            wlanapi.WlanCloseHandle(handle, None)
    except Exception as e:
        print(f"Avertissement : impossible de forcer le scan matériel ({e})")

def scan_arduinode_wifis():
    """ Balaye les réseaux Wi-Fi environnants et extrait tous les SSIDs qui correspondent au format 'Arduinode_X_WIFI'."""
    arduinode_list = []

    try:   

        while not arduinode_list:
            force_wifi_scan()
            time.sleep(2.5)

            result = subprocess.run(
                "netsh wlan show networks", 
                shell=True, 
                capture_output=True, 
                text=True, 
                encoding="cp1252"
            )
            
            if result.returncode == 0:
                pattern = r"Arduinode_\d+_WIFI"
                matches = re.findall(pattern, result.stdout, re.IGNORECASE)
                arduinode_list = list(set(matches))

            if not arduinode_list:
                print(".", end="", flush=True)
                time.sleep(1)
        
    except Exception as e:
        print(f"Erreur scan Wi-Fi : {e}")
        
    return arduinode_list

def get_unique_filepath(folder, filename):
    base, ext = os.path.splitext(filename)
    counter = 1
    target_path = os.path.join(folder, filename)
    
    while os.path.exists(target_path):
        target_path = os.path.join(folder, f"{base}_{counter}{ext}")
        counter += 1
        
    return target_path

def collect_data_wifi(source, save_mode="CSV + BIN"):
    """ Télécharge tous les fichiers de l'ESP32 par Wi-Fi, les supprime de la SD, puis applique la conversion sélectionnée dans l'IHM."""

    PWD = "password123"
    IP_ESP = "192.168.4.1"
    url_root = f"http://{IP_ESP}"

    available_arduinodes = scan_arduinode_wifis()

    if not available_arduinodes:
        print("Aucune carte Arduinode n'a été détecté (WIFI). Collecte annulée.")
        return 
    
    print(f"\n{len(available_arduinodes)} carte(s) détectée(s). Début de la collecte...")

    # # # Collecte des fichiers des arduinodes # # #

    for ssid in available_arduinodes:

        if connect_to_wifi(ssid):
            connected_to_node = True
        
        else:
            connected_to_node = configure_wifi(ssid, PWD)
        
        if not connected_to_node:
            print(f"Erreur : impossible de basculer sur le WiFi de {ssid}. Passage à la suivante.")
            continue

        try:

            # Récupération de la liste des fichiers
            response = requests.get(f"{url_root}/list", timeout=5)
            if response.status_code != 200 or not response.text.strip():
                print(f"[{ssid}]    Aucun fichier binaire à récupérer (Carte SD vide).")
                
                try:
                    requests.get(f"{url_root}/reboot", timeout=2)
                except requests.RequestException:
                    pass
                
                time.sleep(2)
                continue 
            
            files = response.text.strip().split("\n")
            print(f"[{ssid}]    {len(files)} fichier(s) détecté(s) sur l'Arduinode.")

            temp_folder = "./DUMP_WIFI"
            csv_folder = f"./DATACSV/{files[0][:11]}"
            raw_folder = f"./DATARAW/{files[0][:11]}"

            os.makedirs(temp_folder, exist_ok=True)
            os.makedirs(csv_folder, exist_ok=True)
            os.makedirs(raw_folder, exist_ok=True)

            for file in files:
                file_name = file.lstrip('/')
                path_temp_bin = os.path.join(temp_folder, file_name)
                
                print(f"\n [{ssid}] Téléchargement de : {file_name}...")
                
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

            print(f"\nFin des opérations pour {ssid}.")

            print(f"Demande de réinitialisation à {ssid} (réactivation du BLE)...")
            try:
                requests.get(f"{url_root}/reboot", timeout=2)
            except requests.RequestException:
                pass
            
            time.sleep(2)

        except Exception as e:
            print(f"Erreur lors de la collecte sur {ssid} : {e}")

    if connected_to_node:
        subprocess.run("netsh wlan disconnect", shell=True, stdout=subprocess.DEVNULL)

    # # # Collecte du fichier de la source (si active) # # #
    
    if source:
        source_ssid = "SOURCE_WIFI"
        target_file = "Source.csv"

        if connect_to_wifi(source_ssid):
            connected_to_source = True
        
        else:
            connected_to_source = configure_wifi(source_ssid, PWD)
        
        if not connected_to_source:
            print(f"Erreur : impossible de basculer sur le WiFi de {source_ssid}.")
            return True

        try:
            source_folder = "./SOURCE"
            os.makedirs(source_folder, exist_ok=True)

            path_source = get_unique_filepath(source_folder, target_file)
            print(f"\n [{source_ssid}] Téléchargement de : {target_file}...")
            
            with requests.get(f"{url_root}/download", params={"name": target_file}, stream=True, timeout=5) as r:
                if r.status_code == 404:
                    print(f"[{source_ssid}] Aucun fichier '{target_file}' présent sur la carte SD.")
                elif r.status_code == 200:    
                    with open(path_source, 'wb') as f:
                        for chunk in r.iter_content(chunk_size=8192):
                            f.write(chunk)
                
                    if os.path.exists(path_source) and os.path.getsize(path_source) > 0:
                        print(f"Fichier sauvegardé sous : {path_source}")
                        del_resp = requests.get(f"{url_root}/delete", params={"name": target_file})
                        if del_resp.status_code == 200:
                            print(f"'{target_file}' supprimé avec succès de la SD.")
                    else:
                        print(f"Erreur critique : fichier téléchargé vide.")
            
            print(f"Demande de réinitialisation à {source_ssid} (réactivation du BLE)...")
            try:
                requests.get(f"{url_root}/reboot", timeout=2)
            except requests.RequestException:
                pass
            
            time.sleep(2)

        except Exception as e:
                    print(f"Erreur lors de la collecte sur {source_ssid} : {e}")

        finally:
            subprocess.run("netsh wlan disconnect", shell=True, stdout=subprocess.DEVNULL)

    return True

def start_collect(source, save_mode="CSV + BIN", on_complete=None):
    """Déclenche la récupération dans un thread pour préserver l'IHM."""
    
    def wrapper():
        success = collect_data_wifi(source, save_mode)
        
        if on_complete:
            on_complete()
            
        return success
    
    t = threading.Thread(target=wrapper, daemon=True)
    t.start()