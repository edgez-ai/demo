CREATE TABLE IF NOT EXISTS psk_keys (
    identity VARCHAR(255) PRIMARY KEY,
    psk BLOB NOT NULL,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS managed_psk (
    zone VARCHAR(255) NOT NULL,
    endpoint VARCHAR(255) NOT NULL,
    identity VARCHAR(255) NOT NULL,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (zone, endpoint)
);

CREATE TABLE IF NOT EXISTS luci_main_config (
    zone VARCHAR(255) NOT NULL,
    cfg_key VARCHAR(128) NOT NULL,
    cfg_value VARCHAR(2048),
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (zone, cfg_key)
);

CREATE TABLE IF NOT EXISTS luci_devices (
    zone VARCHAR(255) NOT NULL,
    device_id VARCHAR(255) NOT NULL,
    endpoint VARCHAR(255),
    serial VARCHAR(255),
    name VARCHAR(255),
    sampling_enabled BOOLEAN DEFAULT FALSE,
    sampling_rate INT DEFAULT 60,
    report_rate INT DEFAULT 300,
    sleep_mode VARCHAR(64) DEFAULT 'no',
    sensor_list_uart_i2c VARCHAR(2048),
    sensor_list_rs485 VARCHAR(2048),
    sampling_config_version INT DEFAULT 1,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (zone, device_id)
);

CREATE TABLE IF NOT EXISTS luci_sensors (
    zone VARCHAR(255) NOT NULL,
    sensor_group VARCHAR(64) NOT NULL,
    sensor_id VARCHAR(255) NOT NULL,
    sensor_version VARCHAR(64) NOT NULL DEFAULT '1',
    display_name VARCHAR(255),
    driver_json CLOB,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (zone, sensor_group, sensor_id, sensor_version)
);
