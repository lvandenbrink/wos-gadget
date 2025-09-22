Create test server to for the gadget to communicate with.

Host servers HTTP and MQTT servers with docker
```
docker compuse up -d
```

Listen to MQTT messages
```
mosquitto_sub -v -h locahost -p 1883 -t '#'
```

Send a test message to MQTT server
```
mosquitto_pub -h 127.0.0.1  -m '[{"name":"temp", "value":26.31, "unit":"C"},{"name":"humid", "value":47.7, "unit":"%"}]' -t "wos/measurement"
```