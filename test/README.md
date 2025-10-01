Create test server to for the gadget to communicate with.

Host servers HTTP and MQTT servers with docker
```
docker compuse up -d
```

### Configure MQTT Password

Create a file named `mqtt_password.txt` and add your desired MQTT username and password in the following format:
```
username:password
```

Run:
```
mosquitto_passwd -U mqtt_password.txt
```

This file will be mounted into the Mosquitto container for authentication.

### 
Listen to MQTT messages
```
mosquitto_sub -v -h locahost -p 1883 -t '#'
```

Send a test message to MQTT server
```
mosquitto_pub -h 127.0.0.1  -m '[{"name":"temp", "value":26.31, "unit":"C"},{"name":"humid", "value":47.7, "unit":"%"}]' -t "wos/measurement"
```