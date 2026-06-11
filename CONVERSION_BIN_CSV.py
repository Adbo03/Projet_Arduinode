import struct
import csv
import os
import math
import shutil

# --- CONFIGURATION ---
SD_CARD_PATH = "D:/"

SAMPLE_SIZE = 16

def process_file(bin_path, csv_path):
    """Effectue la conversion d'un fichier binaire unique en CSV."""
    file_size = os.path.getsize(bin_path)
    
    # Valeurs par défaut si le header GPS est absent ou corrompu
    lat, lon = "0.000000", "0.000000"
    range = ""
    real_hours, real_minutes, real_seconds = 0, 0, 0
    last_us = 0

    with open(bin_path, "rb") as bin_file, open(csv_path, "w", newline='') as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["Acc_X_g", "Acc_Y_g", "Acc_Z_g", "Roll", "Pitch", "Latitute", "Longitude", "Time (UTC)"])

        # Vérification du header GPS
        first_bytes = bin_file.peek(4)
        if first_bytes.startswith(b"GPS:"):
            gps_line = bin_file.readline().decode('utf-8').strip()
            parts = gps_line.replace("GPS:", "").split(",")
            if len(parts) >= 4:
                lat, lon, time_raw, range = parts[0], parts[1], parts[2], parts[3]
                try:
                    hours, minutes, seconds = time_raw.split(':')
                    real_hours = int(hours)
                    real_minutes = int(minutes)
                    real_seconds = int(seconds)
                except ValueError:
                    print(f" -> Attention : Format de l'heure GPS invalide dans {os.path.basename(bin_path)}")

        # Lecture des données binaires
        count = 0
        
        while True:
            # Lire 16 octets (X, Y, Z) + microseconds
            data = bin_file.read(SAMPLE_SIZE)
            if len(data) < SAMPLE_SIZE:
                break 

            raw_x, raw_y, raw_z, offset_us = struct.unpack('<iiiI', data)

            # Conversion en 'g'
            if range == "2g":
                SCALE_FACTOR = 256000.0
            
            elif range == "4g":
                SCALE_FACTOR = 128000.0

            elif range == "8g":
                SCALE_FACTOR = 64000.0

            ax = raw_x / SCALE_FACTOR
            ay = raw_y / SCALE_FACTOR
            az = raw_z / SCALE_FACTOR
            try:
                pitch = math.atan2(-ax, math.sqrt(ay * ay + az * az)) * 180.0 / math.pi
                roll = math.atan2(ay, az) * 180.0 / math.pi
            except ValueError:
                pitch, roll = 0.0, 0.0

            real_us = offset_us % 1000000

            if(last_us > real_us):
                real_seconds += 1

            if(real_seconds > 59):
                real_minutes += 1
                real_seconds = 0

            if(real_minutes > 59):
                real_hours += 1
                real_minutes = 0

            if(real_hours > 23):
                real_hours = 0

            timestamp = f"{real_hours:02d}:{real_minutes:02d}:{real_seconds:02d}.{real_us:06d}"
            last_us = real_us

            # Écriture dans le CSV
            writer.writerow([f"{ax:.3f}", f"{ay:.3f}", f"{az:.3f}", f"{roll:.2f}", f"{pitch:.2f}", lat, lon, timestamp])
            
            count += 1
            if count % 80000 == 0: # Progression toutes les 20 secondes de données à 4kHz
                progression = (bin_file.tell() / file_size) * 100
                print(f"Progression : {progression:.1f}% ({count} échantillons)")


def process_batch(save_mode="CSV + BIN"):
    
    if save_mode in ["CSV", "CSV + BIN"]:
        os.makedirs("DATACSV", exist_ok=True)
    if save_mode in ["BIN", "CSV + BIN"]:
        os.makedirs("DATARAW", exist_ok=True)

    # Recherche de tous les fichiers .bin à la racine du répertoire cible
    all_files = os.listdir(SD_CARD_PATH)
    bin_files = [f for f in all_files if f.lower().endswith('.bin') and os.path.isfile(os.path.join(SD_CARD_PATH, f))]

    if not bin_files:
        print("Aucun fichier binaire (.bin) trouvé dans la carte SD.")
        return

    for index, filename in enumerate(bin_files, start=1):
        bin_path = os.path.join(SD_CARD_PATH, filename)
        
        # Génération du nom du fichier CSV correspondant
        name_without_ext = os.path.splitext(filename)[0]
        csv_filename = f"{name_without_ext}.csv"
        csv_path = os.path.join("DATACSV", csv_filename)
        dest_raw_path = os.path.join("DATARAW", filename)

        print(f"[{index}/{len(bin_files)}] Traitement de : {filename}")
        
        try:
            if save_mode == "CSV":
                process_file(bin_path, csv_path)
                os.remove(bin_path)

            elif save_mode == "BIN":
                shutil.move(bin_path, dest_raw_path)

            elif save_mode == "CSV + BIN":
                process_file(bin_path, csv_path)
                shutil.move(bin_path, dest_raw_path)

        except Exception as e:
            print(f" /!\\ Erreur lors du traitement de {filename} : {e}\n")

    print("--- Opération de traitement par lots terminée ---")

if __name__ == "__main__":
    process_batch()