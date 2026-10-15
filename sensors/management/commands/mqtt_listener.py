import json
import ssl
from django.core.management.base import BaseCommand
import paho.mqtt.client as mqtt
from sensors.models import TelemetryLog, AlarmEvent

class Command(BaseCommand):
    help = 'Starts the MQTT listener to ingest gas sensor data'

    def handle(self, *args, **kwargs):
        broker = "k68f60c1.ala.asia-southeast1.emqxsl.com"
        port = 8883
        username = "Van"
        password = "123qweasd"

        # Using paho-mqtt v2 Callback API
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="Django_Backend_Listener")
        client.username_pw_set(username, password)
        
        # Enable secure TLS connection using system default certificates
        client.tls_set(cert_reqs=ssl.CERT_REQUIRED, tls_version=ssl.PROTOCOL_TLS)

        def on_connect(client, userdata, flags, reason_code, properties):
            if reason_code == 0:
                self.stdout.write(self.style.SUCCESS("Connected to EMQX Broker!"))
                client.subscribe("elective3/gas_node/data")
                client.subscribe("elective3/gas_node/alarm")
            else:
                self.stdout.write(self.style.ERROR(f"Connection failed: {reason_code}"))

        def on_message(client, userdata, msg):
            try:
                payload = json.loads(msg.payload.decode('utf-8'))
                
                if msg.topic == "elective3/gas_node/data":
                    TelemetryLog.objects.create(
                        mq2_raw=payload['mq2']['raw'],
                        mq2_avg=payload['mq2']['avg'],
                        mq2_thresh=payload['mq2']['thresh'],
                        mq7_rise=payload['mq7']['rise'],
                        mq135_rise=payload['mq135']['rise']
                    )
                    self.stdout.write("Saved telemetry to database.")

                elif msg.topic == "elective3/gas_node/alarm":
                    AlarmEvent.objects.create(
                        source=payload.get('source', 'Unknown'),
                        status=payload['status']
                    )
                    self.stdout.write(self.style.WARNING(f"Logged ALARM: {payload['status']}"))

            except Exception as e:
                self.stdout.write(self.style.ERROR(f"Error parsing message: {e}"))

        client.on_connect = on_connect
        client.on_message = on_message

        self.stdout.write("Connecting to secure broker...")
        client.connect(broker, port, 60)
        
        try:
            client.loop_forever()
        except KeyboardInterrupt:
            self.stdout.write("Listener stopped.")