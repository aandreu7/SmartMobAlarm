import serial
import time
import os
import cv2
import numpy as np
import insightface
from insightface.app import FaceAnalysis
from datetime import datetime, timezone
from PIL import Image, ImageChops, ImageStat
import io
import requests
import warnings
import asyncio 
from bleak import BleakClient, BleakScanner 
import socketio 
import uuid 

# --- LIBRERÍAS CLOUD ---
import cloudinary
import cloudinary.uploader
from azure.cosmos import CosmosClient
from dotenv import load_dotenv, find_dotenv

load_dotenv(find_dotenv(), override=True)

# --- CLIENTE SOCKET.IO (UI REAL-TIME) ---
sio = socketio.AsyncClient()

@sio.event
async def connect():
    print("✅ [CALLBACK] Socket.IO: ¡Conexión establecida!")

@sio.event
async def connect_error(data):
    print(f"❌ [CALLBACK] Socket.IO: Error de conexión -> {data}")

@sio.event
async def disconnect():
    print("⚠️ [CALLBACK] Socket.IO: Desconectado")

# --- CONFIGURACIÓN CLOUDINARY ---
cloudinary.config(
    cloud_name = os.getenv("CLOUDINARY_CLOUD_NAME"),
    api_key    = os.getenv("CLOUDINARY_API_KEY"),
    api_secret = os.getenv("CLOUDINARY_API_SECRET"),
    secure     = True
)

# --- CONFIGURACIÓN COSMOS DB ---
try:
    cosmos_client = CosmosClient(os.getenv("COSMOS_ENDPOINT"), os.getenv("COSMOS_KEY"))
    database = cosmos_client.get_database_client(os.getenv("COSMOS_DB_NAME"))
    container = database.get_container_client(os.getenv("COSMOS_CONTAINER_NAME"))
    print("☁️ Conexión a Cosmos DB: OK")
except Exception as e:
    print(f"❌ Error conectando a Cosmos DB (Revisa el .env): {e}")
    container = None

# --- CONFIGURACIÓN LOCAL ---
TARGET_PORT = "COM7"   
BAUD_RATE = 115200     
OUTPUT_DIR = "."       
KNOWN_FACES_DIR = "personas_conocidas"

# Identificadores
EDGE_ID = os.getenv("EDGE_ID", "Edge_Default")
DEVICE_ID = os.getenv("DEVICE_ID", "Device_Default")

# --- CONFIGURACIÓN BLE (WATCHDOG) ---
# Nombre del dispositivo BLE asociado al Watchdog
WATCHDOG_BLE_NAME = "WATCHDOG" 
# UUID de la característica que envía los datos
CHARACTERISTIC_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"

# UMBRALES
SIMILARITY_THRESHOLD = 0.5 # Positivo si el detector facial detecta una cara desconocida con confianza inferior al threshold   
VISUAL_DIFF_PERCENTAGE = 20.0 # Positivo si entre la imagen de referencia y la de evento la diferencia porcentual es superior al threshold
AUDIO_LOUD_THRESHOLD = 2000 # Positivo si el nivel de sonido detectado es superior al threshold
AUDIO_WINDOW_SEC = 30 # Positivo si se detectan tantas alarmas de audio en menor tiempo que el threshold
AUDIO_MAX_COUNT = 2 # Positivo si se detectan tantas alarmas de audio en menor tiempo que el threshold        

# --- ESTADO GLOBAL ---
audio_timestamps = [] 
latest_ble_data = "Waiting for BLE..."

# --- INICIALIZACIÓN IA ---
print("🧠 Inicializando InsightFace...")
app = FaceAnalysis(name='buffalo_l', providers=['CPUExecutionProvider'])
app.prepare(ctx_id=0, det_size=(640, 640))

known_embeddings = []
known_names = []

def load_known_faces():
    print("📂 Cargando caras conocidas...")
    if not os.path.exists(KNOWN_FACES_DIR):
        os.makedirs(KNOWN_FACES_DIR)
        return

    for filename in os.listdir(KNOWN_FACES_DIR):
        if filename.lower().endswith((".jpg", ".png", ".jpeg")):
            path = os.path.join(KNOWN_FACES_DIR, filename)
            img = cv2.imread(path)
            if img is None: continue
            
            faces = app.get(img)
            if len(faces) > 0:
                known_embeddings.append(faces[0].embedding)
                known_names.append(os.path.splitext(filename)[0])
                print(f"   ✅ Aprendido: {os.path.splitext(filename)[0]}")
    print(f"🧠 Total aprendido: {len(known_names)}\n")

def compute_sim(feat1, feat2):
    return np.dot(feat1, feat2) / (np.linalg.norm(feat1) * np.linalg.norm(feat2))

def get_visual_difference_percentage_from_source(ref_source, event_bytes):
    try:
        if not ref_source: return 0.0

        if isinstance(ref_source, str) and ref_source.startswith("http"):
            resp = requests.get(ref_source, timeout=10)
            resp.raise_for_status()
            ref_img = Image.open(io.BytesIO(resp.content)).convert('RGB')
        else:
            ref_img = Image.open(ref_source).convert('RGB')

        event_img = Image.open(io.BytesIO(event_bytes)).convert('RGB')

        if ref_img.size != event_img.size:
            event_img = event_img.resize(ref_img.size)

        diff = ImageChops.difference(ref_img, event_img)
        stat = ImageStat.Stat(diff)
        diff_percent = (sum(stat.mean) / (len(stat.mean) * 255)) * 100
        return diff_percent
    except Exception as e:
        print(f"⚠️ Error comparando imágenes: {e}")
        return 0.0

def upload_to_cloud_ecosystem(verdict, reasons, img_path=None, img_bytes=None, event_type="EVENT", name_hint=None):
    image_url = "NO_IMAGE"

    # 1. SUBIR IMAGEN
    if img_bytes is not None:
        print("☁️ Subiendo imagen (bytes)...", end=" ")
        try:
            stream = io.BytesIO(img_bytes)
            name_without_ext = name_hint if name_hint else datetime.now().strftime('%Y%m%d_%H%M%S')
            response = cloudinary.uploader.upload(
                stream,
                public_id=f"{EDGE_ID}/{name_without_ext}",
                folder="security_events",
                type="authenticated"
            )
            image_url = response.get("secure_url", image_url)
            print("✅ OK")
        except Exception as e:
            print(f"❌ Fallo subida: {e}")
    elif img_path and os.path.exists(img_path):
        print("☁️ Subiendo imagen (archivo)...", end=" ")
        try:
            filename = os.path.basename(img_path)
            name_without_ext = os.path.splitext(filename)[0]
            response = cloudinary.uploader.upload(
                img_path,
                public_id=f"{EDGE_ID}/{name_without_ext}", 
                folder="security_events",
                type="authenticated"
            )
            image_url = response.get("secure_url", image_url)
            print("✅ OK")
        except Exception as e:
            print(f"❌ Fallo subida: {e}")

    # 2. SUBIR METADATOS A COSMOS DB
    if container:
        print("☁️ Guardando en Cosmos DB...", end=" ")
        try:
            document = {
                "id": str(uuid.uuid4()),     
                "edge_id": EDGE_ID,          
                "device_id": DEVICE_ID,
                "type": event_type, # Si se trata de la Referencia Inicial o de un Evento
                "timestamp": datetime.now(timezone.utc).isoformat(),
                "verdict": verdict, # Si la alarma ha sido clasificada como positiva o negativa       
                "reasons": reasons, # Razones de la clasificación
                "image_url": image_url, # URL de la imagen subida a Cloudinary
                "telemetry_snapshot": latest_ble_data # Guardamos el último dato BLE conocido
            }
            container.create_item(body=document)
            print("✅ Cosmos DB OK")
        except Exception as e:
            print(f"❌ Fallo Cosmos DB: {e}")

# --- CALLBACK DE NOTIFICACIÓN BLE ---
# Esta función se ejecuta automáticamente cada vez que el Watchdog envía datos nuevos
async def notification_handler(sender, data):
    global latest_ble_data
    # Convertir bytes a string
    try:
        decoded_str = data.decode('utf-8')
        latest_ble_data = decoded_str
        # Imprimir en una línea fija o log (opcional) para ver qué llega
        print(f"📡 BLE TELEMETRY: {latest_ble_data}") 
        
        # [SOCKET.IO] Enviar evento al servidor Node.js (UI)
        # Esto permite que la web se actualice en TIEMPO REAL sin esperar
        if sio.connected:
            await sio.emit('telemetry_data', decoded_str)
        else:
            print("⚠️ Socket.IO desconectado. No se envían datos.")

    except Exception as e:
        print(f"Error decodificando BLE: {e}")

# --- BUCLE DE CONEXIÓN SOCKET.IO (RECONEXIÓN AUTOMÁTICA) ---
async def socket_connection_loop():
    """
    Este bucle supervisa constantemente la conexión con el servidor Socket.IO (UI).
    Si detecta que está desconectado (o si la conexión inicial falla), intenta
    conectar de nuevo cada 5 segundos. Esto asegura que si el servidor se reinicia,
    el Edge se reconecta automáticamente sin intervención.
    """
    print("🔄 Iniciando gestor de conexión Socket.IO...")
    while True:
        if not sio.connected:
            try:
                # Intentamos conectar (dejamos que negocie transporte automáticamente)
                await sio.connect('http://127.0.0.1:3001')
                print("✅ Socket.IO CONECTADO (Loop).")
            except Exception as e:
                # Si falla (ej: servidor apagado), esperamos y reintentamos
                print(f"⚠️ Servidor UI no disponible: {e}") 
                pass
        await asyncio.sleep(5)

# --- BUCLE BLE ASÍNCRONO ---
async def ble_telemetry_loop():
    """
    Esta función se encarga de gestionar la conexión con el Watchdog a través de BLE, aunque no recoge los datos de los sensores directamente.

    Para la recogida de datos del Watchdog, se settea como callback la función notification_handler, encargada de recoger los datos
    por BLE y guardaros en la variable last_ble_data.

    Para gestionar la conexión, esta función se conecta al dispositivo Watchdog mediante BLE, identificándole por su nombre asignado,
    y se suscribe a la característica de Notificación, para que notification_handler pueda encargar de recoger las notificaciones.

    También revisa periódicamente el estado de la conexión, por si esta se cerrara. En tal caso, se reintentaría reconectar.
    """
    print("🔵 Iniciando Escaneo BLE para Watchdog...")
    while True:
        try:
            # Buscar dispositivo por nombre
            device = await BleakScanner.find_device_by_name(WATCHDOG_BLE_NAME, timeout=10.0)
            if device:
                print(f"🔵 Watchdog encontrado: {device.address}. Conectando...")
                async with BleakClient(device) as client:
                    print("✅ Conectado a Watchdog BLE via Edge!")
                    
                    # Suscribirse a notificaciones
                    await client.start_notify(CHARACTERISTIC_UUID, notification_handler)
                    
                    # Mantener conexión viva
                    while client.is_connected:
                        # Pausa para ceder momentáneamente el control al bucle Bluetooth (BT).
                        # Al estar en un bucle con condición "True" (se ejecuta indefinidamente), asyncio, al trabajar en un único hilo,
                        # se quedaría detenido en este bucle para siempre, por lo que no se daría tiempo de CPU a los demás trabajos asignados
                        # a asyncio. Esta pausa permite darle tiempo de CPU a los demás trabajos. 
                        await asyncio.sleep(1) # Revisa la conexión tras X segundos
                        
                print("⚠️ Desconectado de Watchdog BLE. Reintentando...")
            else:
                print("⚠️ Watchdog BLE no encontrado. Re-escaneando en 5s...")
                await asyncio.sleep(5) # Ofrece varios segundos a los demás trabajos asignados a Asyncio mientras intenta recuperar la conexión BLE.
                
        except Exception as e:
            print(f"❌ Error Bucle BLE: {e}")
            await asyncio.sleep(5)

# --- BUCLE SERIAL PRINCIPAL (Ahora asíncrono) ---
async def serial_read_loop():
    """
    Se encarga de la gestión de eventos.

    Recibe información del Reporter a través de Bluetooth (BT).

    Esta información puede ser:
        - Estado Inicial: 
            imagen de referencia, que se usará para comparar las demás imágenes que lleguen.
        - Reporte de Eventos: 
            cuando los sensores detectan valores inusuales, la Reporter captura imagen y la envía junto 
            con los valores de los sensores. Esto es un Evento. Este Evento debe ser clasificado como
            Positivo o Negativo, en función de las reglas y los thresholds establecidos.

    """
    global audio_timestamps
    
    print(f"🔌 Conectando Serial a {TARGET_PORT}...")
    ser = None # Variable que representa la conexión por BT con el Reporter
    try:
        ser = serial.Serial(TARGET_PORT, BAUD_RATE, timeout=0.1) # Timeout bajo para no bloquear asyncio
        print("✅ Serial Conectado.")
    except Exception as e:
        print(f"❌ Error Serial: {e}")
        return

    current_state = "IDLE"
    photo_bytes = bytearray()
    raw_type = "UNKNOWN"
    ref_img_path = os.path.join(OUTPUT_DIR, "referencia_inicial.jpg")

    while True:
        # Pausa para ceder momentáneamente el control al bucle BLE.
        # Al estar en un bucle con condición "True" (se ejecuta indefinidamente), asyncio, al trabajar en un único hilo,
        # se quedaría detenido en este bucle para siempre, por lo que no se daría tiempo de CPU a los demás trabajos asignados
        # a asyncio. Esta pausa permite darle tiempo de CPU a los demás trabajos. 
        await asyncio.sleep(0.01) 
        
        try:
            # Lectura no bloqueante simulada
            # Lee los datos recibidos del Reporter a través de Bluetooth (BT)
            # La lectura de la conexión Bluetooth (BT) a través de Serial es bloqueante,
            # por lo tanto, antes de leer, nos aseguramos con ser.in_waiting > 0 que hay datos
            # en la conexión esperando a ser leídos, lo que minimiza el tiempo total de bloqueo.  
            if ser.in_waiting > 0:
                raw_line = ser.readline()
                if not raw_line: continue
                try: text_line = raw_line.decode('utf-8', errors='ignore').strip()
                except: text_line = ""

                # --- LOGICA ORIGINAL DE PARSEO ---
                
                # El bucle escucha Eventos que se detectan en el Device, y que el Reporter manda al Edge a través de Bluetooth (BT)
                #
                # Un Evento puede ser de dos tipos: Referencia Inicial (recibe una imagen de referencia, no hay que clasificarlo) 
                # o Detección (se han detectado valores inusuales, sí hay que clasificarlo)
                #
                # La variable current_state tiene tres valores diferentes:
                #
                # IDLE --> Cuando no hay datos en proceso de ser leídos. La conexión está "tranquila", y el bucle se 
                #           mantiene a la escucha para recibir nuevos Eventos 
                #
                # METADATA --> Cuando se ha recibido un nuevo Evento, lo primero es leer sus metadatos (valor de sensores y causa del Evento)
                #
                # PHOTO --> Cuando se ha recibido un nuevo Evento y ya se han leído los metadatos, se procede a leer la imagen completa (puede tardar varias iteraciones)

                # 1. CABECERA
                if "=== INCIDENT REPORT ===" in text_line:
                    current_state = "METADATA"
                    photo_bytes = bytearray()
                    continue

                # 2. TIPO
                if current_state == "METADATA" and text_line.startswith("TYPE:"):
                    raw_type = text_line.replace("TYPE:", "").strip()
                    if raw_type == "INITIAL_REFERENCE":
                        print("\n📸 Recibiendo Referencia Inicial...")
                    else:
                        print(f"\n🚨 EVENTO RECIBIDO: {raw_type}")
                        print(f"   (Telemetría BLE actual: {latest_ble_data})") # Mostrar dato BLE asociado
                    continue

                # 3. FOTO
                if "--- FOTO START ---" in text_line:
                    print("📸 Descargando...", end="", flush=True)
                    current_state = "PHOTO"
                    continue
                
                if current_state == "PHOTO":
                    if b'--- FOTO END ---' in raw_line:
                        print(" OK.")
                        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                        event_bytes = bytes(photo_bytes)

                        if raw_type == "INITIAL_REFERENCE":
                            print("✅ Guardando Referencia local...")
                            try:
                                with open(ref_img_path, "wb") as rf: rf.write(event_bytes)
                                upload_to_cloud_ecosystem("INFO", ["Nueva Referencia"], img_path=ref_img_path, event_type="REFERENCE", name_hint=f"ref_{timestamp}")
                            except Exception as e: print(f"❌ Error ref: {e}")
                        else:
                            # --- LÓGICA DE DECISIÓN ---
                            is_positive = False
                            reasons = []
                            parts = raw_type.split(":")
                            category = parts[1] if len(parts) > 1 else "UNKNOWN"
                            val_str = parts[2] if len(parts) > 2 else "0"
                            sensor_val = int(val_str)

                            # 1. SENSORES
                            if "IMU" in category:
                                is_positive = True
                                reasons.append(f"IMU Activada ({category})")

                            if "AUDIO" in category:
                                if sensor_val > AUDIO_LOUD_THRESHOLD:
                                    is_positive = True
                                    reasons.append(f"Audio Intenso ({sensor_val})")
                                now = time.time()
                                audio_timestamps.append(now)
                                audio_timestamps = [t for t in audio_timestamps if now - t <= AUDIO_WINDOW_SEC]
                                if len(audio_timestamps) >= AUDIO_MAX_COUNT:
                                    is_positive = True
                                    reasons.append(f"Frecuencia Audio ({len(audio_timestamps)} en 30s)")

                            # 2. VISUAL
                            print("🔍 Analizando imagen...")
                            img = None
                            try:
                                nparr = np.frombuffer(event_bytes, np.uint8)
                                img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
                            except: img = None

                            faces = []
                            if img is not None: faces = app.get(img)

                            if len(faces) > 0:
                                intruder_emb = faces[0].embedding
                                match_found = False
                                matched_name = "Desconocido"
                                for idx, known_emb in enumerate(known_embeddings):
                                    sim = compute_sim(intruder_emb, known_emb)
                                    if sim > SIMILARITY_THRESHOLD:
                                        match_found = True
                                        matched_name = known_names[idx]
                                        break
                                if not match_found:
                                    is_positive = True
                                    reasons.append("Cara NO Registrada")
                                else:
                                    print(f"   -> Cara conocida: {matched_name}")
                                    is_positive = False 
                                    reasons.append(f"Identificado ({matched_name})")
                            else:
                                print("   -> No hay caras.")
                                diff_pct = 0.0
                                if os.path.exists(ref_img_path):
                                    diff_pct = get_visual_difference_percentage_from_source(ref_img_path, event_bytes)
                                print(f"   -> Cambio Visual: {diff_pct:.2f}%")
                                if diff_pct > VISUAL_DIFF_PERCENTAGE:
                                    is_positive = True
                                    reasons.append(f"Cambio Visual > {VISUAL_DIFF_PERCENTAGE}%")

                            # 3. VEREDICTO
                            verdict = "POSITIVE" if is_positive else "NEGATIVE"
                            print("="*40)
                            print(f"📊 VEREDICTO: {verdict}")
                            if is_positive: print(f"🚨 MOTIVOS: {', '.join(reasons)}")
                            print("="*40)

                            upload_to_cloud_ecosystem(verdict, reasons, img_bytes=event_bytes, name_hint=f"evidencia_{timestamp}")

                        current_state = "IDLE"
                    else:
                        photo_bytes.extend(raw_line)
        
        except Exception as e:
            print(f"Error Bucle Serial: {e}")
            break

# --- MAIN ASÍNCRONO ---
async def main_async():
    load_known_faces()

    # Ejecutar tareas en paralelo:
    # 1. Leer BLE (Watchdog) -> Envía datos por Websocket
    # 2. Leer Serial (Wrover) -> Recibe fotos y alertas
    # 3. Mantener conexión Socket.IO (Reconexión automática)
    await asyncio.gather(
        ble_telemetry_loop(),
        serial_read_loop(),
        socket_connection_loop()
    )

if __name__ == "__main__":
    try:
        asyncio.run(main_async())
    except KeyboardInterrupt:
        print("\n👋 Cerrando sistema...")