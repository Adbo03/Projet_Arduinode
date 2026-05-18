import struct
import csv
import os
import math

# --- CONFIGURATION ---
FILE_BIN = "dataRaw.bin"
FILE_CSV = "dataNode.csv"

# Facteur d'échelle pour l'ADXL355 (+/- 2g) : 256 000 LSB/g
SCALE_FACTOR = 256000.0

def convert_bin_to_csv():
    if not os.path.exists(FILE_BIN):
        print(f"Erreur : Le fichier {FILE_BIN} est introuvable.")
        return

    print(f"Ouverture de {FILE_BIN}...")
    
    file_size = os.path.getsize(FILE_BIN)

    # Taille d'un échantillon : 16 octets
    sample_size = 16
    
    with open(FILE_BIN, "rb") as bin_file, open(FILE_CSV, "w", newline='') as csv_file:
        writer = csv.writer(csv_file)

        writer.writerow(["Acc_X_g", "Acc_Y_g", "Acc_Z_g", "Roll", "Pitch", "Latitute", "Longitude", "Time"])

        first_bytes = bin_file.peek(4)
        if first_bytes.startswith(b"GPS:"):
            gps_line = bin_file.readline().decode('utf-8').strip()
            parts = gps_line.replace("GPS:", "").split(",")
            if len(parts) >= 3:
                lat, lon, time_raw = parts[0], parts[1], parts[2]
            
            hours, minutes, seconds = time_raw.split(':')
            real_hours = int(hours)
            real_minutes = int(minutes)
            real_seconds = int(seconds)
            last_us = 0

        # Lecture des données binaires
        count = 0
        print("Conversion en cours...")
        
        while True:
            # Lire 16 octets (X, Y, Z) + microseconds
            data = bin_file.read(sample_size)
            if len(data) < sample_size:
                break 

            # Dépaquetage du binaire : 
            # '<' : petit-boutiste (little-endian, standard ESP32)
            # 'iii' : trois entiers signés de 4 octets (int32_t)
            raw_x, raw_y, raw_z, offset_us = struct.unpack('<iiiI', data)

            # Conversion en 'g'
            ax = raw_x / SCALE_FACTOR
            ay = raw_y / SCALE_FACTOR
            az = raw_z / SCALE_FACTOR
            pitch = math.atan2(-ax, math.sqrt(ay * ay + az * az)) * 180.0 / math.pi
            roll = math.atan2(ay, az) * 180.0 / math.pi

            # On ajoute l'offset en ajustant si les micros dépassent 1 000 000

            real_us = offset_us % 1000000
            
            if(last_us > real_us):
                real_seconds += 1

            if(real_seconds > 60):
                real_minutes += 1
                real_seconds = 0

            if(real_minutes > 60):
                real_hours += 1
                real_minutes = 0

            if(real_hours > 23):
                real_hours = 0

            timestamp = f"{real_hours:02d}:{real_minutes:02d}:{real_seconds:02d}.{real_us:06d}"

            last_us = real_us

            # Écriture dans le CSV
            writer.writerow([f"{ax:.3f}", f"{ay:.3f}", f"{az:.3f}", f"{roll:.2f}", f"{pitch:.2f}", lat, lon, timestamp])
            
            count += 1
            if count % 40000 == 0: # Progression toutes les 10 secondes de données à 4kHz
                progression = (bin_file.tell() / file_size) * 100
                print(f"Progression : {progression:.1f}% ({count} échantillons)")

    print(f"\nTerminé ! {count} échantillons convertis dans {FILE_CSV}.")

if __name__ == "__main__":
    convert_bin_to_csv()