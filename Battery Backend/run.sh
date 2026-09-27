#!/usr/bin/with-contenv bashio

export API_KEY="$(bashio::config 'api_key')"
export MQTT_HOST="$(bashio::services mqtt 'host')"
export MQTT_PORT="$(bashio::services mqtt 'port')"
export MQTT_USERNAME="$(bashio::services mqtt 'username')"
export MQTT_PASSWORD="$(bashio::services mqtt 'password')"
export DB_PATH="/data/batteries.db"

cd /app
exec python3 main.py
