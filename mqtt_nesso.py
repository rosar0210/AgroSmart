import json
import time

import paho.mqtt.client as mqtt
import psycopg2

# ---------- Configuracion ----------
PG_HOST = "localhost"
PG_PORT = 5432
PG_DB = "agrosmart"
PG_USER = "postgres"
PG_PASS = "postgres"  # ajusta si usas otro usuario/contrasena

MQTT_BROKER = "localhost"
MQTT_PORT = 1883
# Escucha los uplinks de TODAS las aplicaciones y TODOS los dispositivos
MQTT_TOPIC = "application/+/device/+/event/up"

CAMPO = "soil_moisture"
# -----------------------------------

conn = None


def conectar_db():
    """Abre la conexion a PostgreSQL (una sola vez) y crea la tabla si no existe."""
    global conn
    conn = psycopg2.connect(
        host=PG_HOST, port=PG_PORT, database=PG_DB, user=PG_USER, password=PG_PASS
    )
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute(
            """
            CREATE TABLE IF NOT EXISTS soil_moisture_telemetry (
                id SERIAL PRIMARY KEY,
                time TIMESTAMPTZ NOT NULL DEFAULT now(),
                device_eui TEXT NOT NULL,
                device_name TEXT,
                soil_moisture DOUBLE PRECISION
            );
            """
        )
    print("[DB] Conectado a PostgreSQL.")


def insertar(dev_eui, dev_name, humedad):
    """Inserta un registro; si la conexion se ha caido, reconecta y reintenta una vez."""
    global conn
    for intento in range(2):
        try:
            if conn is None or conn.closed:
                conectar_db()
            with conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO soil_moisture_telemetry (device_eui, device_name, soil_moisture) "
                    "VALUES (%s, %s, %s);",
                    (dev_eui, dev_name, humedad),
                )
            return True
        except psycopg2.Error as e:
            print(f"[DB ERROR] {e}")
            conn = None
            time.sleep(1)
    return False


def extraer_humedad(data):
    """Busca el campo en la raiz del JSON o dentro de 'object'."""
    if CAMPO in data:
        return float(data[CAMPO])
    obj = data.get("object") or {}
    if CAMPO in obj:
        return float(obj[CAMPO])
    return None


def on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        print("[MQTT] Conectado al broker.")
        client.subscribe(MQTT_TOPIC)
        print(f"[MQTT] Escuchando: {MQTT_TOPIC}")
    else:
        print(f"[MQTT] Error de conexion: {reason_code}")


def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload.decode("utf-8"))
        dev_info = data.get("deviceInfo", {})
        dev_eui = str(dev_info.get("devEui", "")).lower()
        dev_name = dev_info.get("deviceName", "desconocido")

        humedad = extraer_humedad(data)
        if humedad is None:
            print(f"[WARN] {dev_name} ({dev_eui}): sin campo '{CAMPO}' en el payload.")
            return

        print(f"[DATA] {dev_name} ({dev_eui}) -> Humedad: {humedad}%")
        if insertar(dev_eui, dev_name, humedad):
            print("[DB OK] Registro insertado.")

    except Exception as e:
        print(f"[ERROR] {e}")


if __name__ == "__main__":
    conectar_db()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message

    print("Arrancando escucha LoRaWAN...")
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_forever()  # reconecta solo si se cae el broker
