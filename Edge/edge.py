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
warnings.filterwarnings("ignore", category=FutureWarning)
import uuid # Para generar IDs únicos en la BBDD

# --- LIBRERÍAS CLOUD ---
import cloudinary
import cloudinary.uploader
from azure.cosmos import CosmosClient
from dotenv import load_dotenv, find_dotenv

load_dotenv(find_dotenv(), override=True)

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

# Identificadores para partición
EDGE_ID = os.getenv("EDGE_ID", "Edge_Default")
DEVICE_ID = os.getenv("DEVICE_ID", "Device_Default")

# UMBRALES
SIMILARITY_THRESHOLD = 0.5    
VISUAL_DIFF_PERCENTAGE = 20.0 
AUDIO_LOUD_THRESHOLD = 2000   
AUDIO_WINDOW_SEC = 30         
AUDIO_MAX_COUNT = 2           

# --- ESTADO GLOBAL ---
audio_timestamps = [] 

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

def get_visual_difference_percentage(ref_path, current_path):
    try:
        if not ref_path or not os.path.exists(ref_path): return 0.0
        img1 = Image.open(ref_path).convert('RGB')
        img2 = Image.open(current_path).convert('RGB')
        if img1.size != img2.size: img2 = img2.resize(img1.size)
        diff = ImageChops.difference(img1, img2)
        stat = ImageStat.Stat(diff)
        diff_percent = (sum(stat.mean) / (len(stat.mean) * 255)) * 100
        return diff_percent
    except Exception as e:
        print(f"⚠️ Error comparando imágenes: {e}")
        return 0.0

def get_visual_difference_percentage_from_source(ref_source, event_bytes):
    """ref_source puede ser una URL o una ruta local; event_bytes son los bytes de la imagen del evento."""
    try:
        # Cargar imagen de referencia
        if not ref_source:
            return 0.0

        if isinstance(ref_source, str) and ref_source.startswith("http"):
            resp = requests.get(ref_source, timeout=10)
            resp.raise_for_status()
            ref_img = Image.open(io.BytesIO(resp.content)).convert('RGB')
        else:
            ref_img = Image.open(ref_source).convert('RGB')

        # Cargar imagen de evento desde bytes
        event_img = Image.open(io.BytesIO(event_bytes)).convert('RGB')

        if ref_img.size != event_img.size:
            event_img = event_img.resize(ref_img.size)

        diff = ImageChops.difference(ref_img, event_img)
        stat = ImageStat.Stat(diff)
        diff_percent = (sum(stat.mean) / (len(stat.mean) * 255)) * 100
        return diff_percent
    except Exception as e:
        print(f"⚠️ Error comparando imágenes desde fuente: {e}")
        return 0.0

def get_latest_reference_image_url():
    """Consulta Cosmos DB por la última referencia (type='REFERENCE') y devuelve su image_url si existe."""
    if not container:
        return None
    try:
        query = f"SELECT TOP 1 c.image_url FROM c WHERE c.edge_id = '{EDGE_ID}' AND c.device_id = '{DEVICE_ID}' AND c.type = 'REFERENCE' ORDER BY c.timestamp DESC"
        items = list(container.query_items(query=query, enable_cross_partition_query=True))
        if len(items) > 0:
            return items[0].get('image_url')
    except Exception as e:
        print(f"⚠️ Error consultando referencia en Cosmos DB: {e}")
    return None

def upload_to_cloud_ecosystem(verdict, reasons, img_path=None, img_bytes=None, event_type="EVENT", name_hint=None):
    """
    Sube la imagen (desde ruta o bytes) a Cloudinary y los metadatos a Cosmos DB.
    """
    image_url = "NO_IMAGE"

    # 1. SUBIR IMAGEN A CLOUDINARY
    if img_bytes is not None:
        print("☁️ Subiendo imagen (bytes) a Cloudinary...", end=" ")
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
            print(f"❌ Fallo subida imagen (bytes): {e}")
    elif img_path and os.path.exists(img_path):
        print("☁️ Subiendo imagen a Cloudinary...", end=" ")
        try:
            filename = os.path.basename(img_path)
            name_without_ext = os.path.splitext(filename)[0]
            response = cloudinary.uploader.upload(
                img_path,
                public_id=f"{EDGE_ID}/{name_without_ext}", # Carpeta organizada por Edge
                folder="security_events",
                type="authenticated"
            )
            image_url = response.get("secure_url", image_url)
            print("✅ OK")
        except Exception as e:
            print(f"❌ Fallo subida imagen: {e}")
    else:
        print("☁️ No hay imagen para subir (se ignorará).")

    # 2. SUBIR METADATOS A COSMOS DB
    if container:
        print("☁️ Guardando evento en Cosmos DB...", end=" ")
        try:
            document = {
                "id": str(uuid.uuid4()),     # ID único obligatorio
                "edge_id": EDGE_ID,          # Partition Key Nivel 1
                "device_id": DEVICE_ID,      # Partition Key Nivel 2 (Device)
                "type": event_type,          # EVENT o REFERENCE
                "timestamp": datetime.now(timezone.utc).isoformat(),
                "verdict": verdict,          # POSITIVE / NEGATIVE / INFO
                "reasons": reasons,          # Array de causas
                "image_url": image_url       # Enlace a Cloudinary
            }
            container.create_item(body=document)
            print("✅ OK")
        except Exception as e:
            print(f"❌ Fallo Cosmos DB: {e}")
    else:
        print("⚠️ Cosmos DB no configurado, saltando registro.")

def main():
    global audio_timestamps
    load_known_faces()
    
    print(f"🔌 Conectando a {TARGET_PORT}...")
    try:
        ser = serial.Serial(TARGET_PORT, BAUD_RATE, timeout=1)
        print("✅ Conectado. Esperando eventos...")
        
        current_state = "IDLE"
        photo_bytes = bytearray()
        raw_type = "UNKNOWN"
        ref_img_path = os.path.join(OUTPUT_DIR, "referencia_inicial.jpg")

        while True:
            try:
                raw_line = ser.readline()
                if not raw_line: continue
                try: text_line = raw_line.decode('utf-8', errors='ignore').strip()
                except: text_line = ""

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

                        # Si es referencia inicial, guardarla localmente y subirla a la nube
                        if raw_type == "INITIAL_REFERENCE":
                            print("✅ Recibida Referencia Inicial: guardando localmente...")
                            try:
                                with open(ref_img_path, "wb") as rf:
                                    rf.write(event_bytes)
                                print("✅ Referencia guardada localmente.")
                                # Subir referencia a la nube
                                upload_to_cloud_ecosystem("INFO", ["Nueva Referencia Inicial"], img_path=ref_img_path, event_type="REFERENCE", name_hint=f"referencia_{timestamp}")
                                print("✅ Referencia subida a la nube.")
                            except Exception as e:
                                print(f"❌ Fallo subiendo/guardando referencia: {e}")
                        else:
                            # --- LÓGICA DE DECISIÓN ESTRICTA ---
                            is_positive = False
                            reasons = []
                            parts = raw_type.split(":")
                            category = parts[1] if len(parts) > 1 else "UNKNOWN"
                            val_str = parts[2] if len(parts) > 2 else "0"
                            sensor_val = int(val_str)

                            # 1. ANÁLISIS SENSÓRICO
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

                            # 2. ANÁLISIS VISUAL
                            print("🔍 Analizando imagen...")
                            img = None
                            try:
                                nparr = np.frombuffer(event_bytes, np.uint8)
                                img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
                            except Exception:
                                img = None

                            faces = []
                            if img is not None:
                                faces = app.get(img)

                            face_detected = len(faces) > 0

                            if face_detected:
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
                                    # Si la cara está registrada, siempre notificar pero NO clasificar como POSITIVE
                                    is_positive = False
                                    reasons.append(f"Persona conocida ({matched_name})")
                            else:
                                print("   -> No se detectaron caras en la imagen.")
                                # Usar la referencia local más reciente (guardada en ref_img_path)
                                diff_pct = 0.0
                                if os.path.exists(ref_img_path):
                                    diff_pct = get_visual_difference_percentage_from_source(ref_img_path, event_bytes)
                                else:
                                    print("⚠️ No hay referencia local disponible para comparar.")

                                print(f"   -> Cambio Visual: {diff_pct:.2f}%")
                                if diff_pct > VISUAL_DIFF_PERCENTAGE:
                                    is_positive = True
                                    reasons.append(f"Cambio Visual > {VISUAL_DIFF_PERCENTAGE}% ({diff_pct:.1f}%)")

                            # 3. VEREDICTO FINAL Y SUBIDA
                            verdict = "POSITIVE" if is_positive else "NEGATIVE"
                            print("="*40)
                            print(f"📊 VEREDICTO: {verdict}")
                            if is_positive:
                                print(f"🚨 MOTIVOS: {', '.join(reasons)}")
                            print("="*40)

                            # SUBIR A CLOUDINARY Y COSMOS DB desde bytes
                            upload_to_cloud_ecosystem(verdict, reasons, img_bytes=event_bytes, name_hint=f"evidencia_{timestamp}")

                        current_state = "IDLE"
                    else:
                        photo_bytes.extend(raw_line)

            except serial.SerialException:
                print("⚠️ Desconexión.")
                break

    except Exception as e:
        print(f"❌ Error Crítico: {e}")
    finally:
        if 'ser' in locals() and ser.is_open: ser.close()

if __name__ == "__main__":
    main()