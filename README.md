# Node-sismique-3C
Développement d'un noeud sismique 3 composantes

Arduinode : ADXL355Z, NANO ESP32, lecteur carte SD (JOY-IT), Ultimate GPS Breakout v3 (adafruit)

Source : ASX11504-SW-R (PUI Audio), NANO ESP32, amplificateur de puissance

IDE utilisé : Arduino IDE v2.3.8

A NOTER : Des resistances et des capacités ont aussi été utilisés pour des opérations intermédiaires diverses. 

Ce projet consiste au développement de nodes sismiques 3C low-cost (objectif : ~100€
par unité), conçu pour répondre aux besoins des applications environnementales, géotechniques
et archéologiques. Basés sur une architecture Arduino, ils intègrent des capteurs
MEMS triaxiaux, un module GPS pour la synchronisation temporelle, une carte SD pour le
stockage de données, et une batterie Li-ion pour l’alimentation.

- **ADXL_I2C.ino** : permet la collecte de données d'un ADXL355Z sur ces 3 axes (X, Y, Z). Ces données sont ensuite formatées et peuvent être affichées sur le serial monitor de l'IDE arduino ET/OU sur une application BLE sur différents type d'appareils (ex : "nRF Connect" sur IOS).
- **ADXL_SPI.ino** : équivalent à ADXL_I2C.ino mais pour le protocole SPI (+ données sur l'inclinaison de l'accéléromètre).
- **SERVEUR_ADXL.py** : code serveur qui permet de se connecter en BLE à la carte arduino et d'afficher en temps réel les données sur les 3 axes ainsi que l'inclinaison (Axe X/ Axe Y) de l'ADXL.
- **CONVERSION_BIN_CSV.py** : code permettant la conversion des fichiers binaires en csv et le stockage sur le PC.
- **ScanEarth.exe** : exécutable facilitant le lancement de l'interface pour la visualisation de données et le paramétrage de l'accéléromètre.
- **SOURCE_BLE.ino** : code gérant la génération de la source. Avec la carte, on peut venir modifier la fréquence du signal généré via l'interface (code SERVEUR_ADXL.py).

**CONSEILS D'UTILISATION**

*ARDUINODE*:

Pour faire fonctionner le système, il suffit de programmer la NANO ESP32 avec l'un des deux codes arduino (ADXL_I2C.ino ou ADXL_SPI.ino). Veillez à bien cabler les connections entre l'accéléromètre et l'arduino !

Protocole I2C : 
- VDDIO (ADXL) relié au 3v3 (NANO).
- VDD (ADXL) relié au 3v3 (NANO).
- GND (ADXL) relié au GND (NANO).
- MISO / ASEL relié au GND.
- SCLK / VSSIO relié au GND (pour forcer le mode I2C).
- CS / SCL va sur la broche SCL de l'Arduino (PA5).
- MOSI / SDA va sur la broche SDA de l'Arduino (PA4).
- Une résistance de pull-up de 4.7kΩ entre SDA et 3.3V.
- Une résistance de pull-up de 4.7kΩ entre SCL et 3.3V.

Protocole SPI :
- VDDIO (ADXL) relié au 3v3 (NANO).
- VDD (ADXL) relié au 3v3 (NANO).
- GND (ADXL) relié au GND (NANO).
- MISO / ASEL relié à PD12.
- SCLK / VSSIO relié à PD13.
- CS / SCL relié à PD10.
- MOSI / SDA relié à PD11.

Pour ce qui est des autres composants du système, voici comment les connecter : 

Lecteur SD : 
- GND (lecteur) relié à GND (NANO).
- 3v3 (lecteur) relié à 3v3 (NANO).
- CS (lecteur) relié à PD9 (NANO).
- MOSI (lecteur) relié à PD23 (NANO).
- CLK (lecteur) relié à PD22 (NANO).
- MISO (lecteur) relié à PD21 (NANO).

Module GPS :
- VIN (GPS) relié à 3v3 (NANO).
- GND (GPS) relié à GND (NANO).
- TX (GPS) relié à RX0 (NANO).
- RX (GPS) relié à TX1 (NANO).
- PPS (GPS) relié à D2 (NANO).
- Une resistance de pull-up de 10kΩ entre EN (GPS) et 3v3 (NANO).

L'ADXL propose différentes précisions de mesure. Pour jouer sur cette dernière, dans le code arduino, il faut modifier le **SCALE_FACTOR** et la valeur écrite dans le regsitre **REG_RANGE** :

- Pour +-2g : **REG_RANGE** = 0x01 | **SCALE_FACTOR** = 256 000.
- Pour +-4g : **REG_RANGE** = 0x02 | **SCALE_FACTOR** = 128 000.
- Pour +-8g : **REG_RANGE** = 0x03 | **SCALE_FACTOR** = 64 000.

*SOURCE*:

Pour générer le signal source, le code utilise le PWM de la carte en guise de porteuse à 40kHz. La source audio ne fonctionnant que sur une plage allant de 5Hz-500Hz, il faut donc filtrer la porteuse pour récupérer le signal réel. Pour cela, un filtre passe-bas est utilisé entre la carte et l'amplificateur de puissance. Ce filtre a pour fréquence de coupure ~338,6 Hz (1kΩ/470nF) pour atténuer les fréquences proches et au delà de la limite de la source.

Cablage :

- PWM (D6/NANO) branché en entrée du filtre PB.
- Sortie du filtre doit être branché en entrée de l'amplificateur de puissance.

**INFOS GENERALES** 

L'envoi en BLE se fait automatiquement et n'a pas besoin d'être paramétré. Sur smartphone, il suffit de se connecter au système (nommé 'ADXL355Z') via une application BLE telle que nRF Connect. Si on veut visualiser les graphes en temps réel, on peut aussi exécuter le code **SERVEUR_ADXL.py** à partir d'un terminal :

    python ./SERVEUR_ADXL.py

(Veuillez vous assurer de bien avoir activé le Bluetooth de l'ordinateur exécutant le code)

Pour générer l'exécutable, il faut installer *pyinstaller*. Ensuite, la ligne de commande est la suivante :

    pyinstaller --onefile SERVEUR_ADXL.py

Il faut s'assurer d'être dans le répertoire des fichiers SERVEUR_ADXL.py et CONVERSION_BIN_CSV.py pour que la commande fonctionne. Si vous voulez changer un exécutable avec un autre nom, il suffit de changer le nom du fichier python avant d'exécuter la ligne de commande.

Fait par : Adam Bounour
