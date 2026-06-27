"""
MQTT -> Kafka bridge.

Subscribes to sensors/+/+/telemetry on Mosquitto, validates the JSON,
produces to Kafka topic sensors.telemetry (key = device_id).
Malformed messages go to sensors.telemetry.dlq.
"""
import json
import logging
import os
import signal
import sys
import time

import paho.mqtt.client as mqtt
from confluent_kafka import Producer

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("bridge")

MQTT_HOST = os.environ.get("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_TOPIC = "sensors/+/+/telemetry"

KAFKA_BOOTSTRAP = os.environ.get("KAFKA_BOOTSTRAP", "redpanda:9092")
KAFKA_TOPIC = "sensors.telemetry"
KAFKA_DLQ = "sensors.telemetry.dlq"

# Sensor-specific fields (temp_c, humidity, eco2_ppm, tvoc_ppb, ...) are
# all optional — different devices send different subsets. Only the
# identity + timestamp are mandatory.
REQUIRED_FIELDS = ("device_id", "ts")

producer = Producer({
    "bootstrap.servers": KAFKA_BOOTSTRAP,
    "client.id": "mqtt-bridge",
    "linger.ms": 50,
    "compression.type": "zstd",
    "enable.idempotence": True,
})


def on_delivery(err, msg):
    if err is not None:
        log.error("delivery failed: %s", err)


def validate(payload: dict) -> bool:
    return all(k in payload for k in REQUIRED_FIELDS)


def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        log.warning("bad payload on %s: %s", msg.topic, exc)
        producer.produce(KAFKA_DLQ, msg.payload, on_delivery=on_delivery)
        producer.poll(0)
        return

    if not validate(payload):
        log.warning("missing fields on %s: %s", msg.topic, payload)
        producer.produce(KAFKA_DLQ, msg.payload, on_delivery=on_delivery)
        producer.poll(0)
        return

    key = str(payload["device_id"]).encode("utf-8")
    producer.produce(
        KAFKA_TOPIC,
        json.dumps(payload).encode("utf-8"),
        key=key,
        on_delivery=on_delivery,
    )
    producer.poll(0)
    log.info("forwarded %s key=%s", msg.topic, payload["device_id"])


def on_connect(client, userdata, flags, reason_code, properties=None):
    log.info("MQTT connected (rc=%s), subscribing %s", reason_code, MQTT_TOPIC)
    client.subscribe(MQTT_TOPIC, qos=1)


def main():
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id="mqtt-kafka-bridge",
    )
    client.on_connect = on_connect
    client.on_message = on_message

    def shutdown(signum, frame):
        log.info("shutting down (signal %s)", signum)
        client.disconnect()
        producer.flush(5)
        sys.exit(0)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    while True:
        try:
            log.info("connecting to MQTT %s:%s", MQTT_HOST, MQTT_PORT)
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
            client.loop_forever()
        except Exception as exc:
            log.error("MQTT loop crashed: %s — retrying in 5s", exc)
            time.sleep(5)


if __name__ == "__main__":
    main()
