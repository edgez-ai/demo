package ai.edgez.server.lwm2m.util;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Comparator;

import com.fasterxml.jackson.core.type.TypeReference;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.context.annotation.Lazy;
import org.springframework.core.io.Resource;
import org.springframework.core.io.support.ResourcePatternResolver;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Service;

@Service
public class LuciConfigService {

    private static final Logger LOG = LoggerFactory.getLogger(LuciConfigService.class);
    private static final TypeReference<Map<String, Object>> MAP_TYPE = new TypeReference<>() {
    };

    private final JdbcTemplate jdbcTemplate;
    private final ResourcePatternResolver resourcePatternResolver;
    private final ObjectMapper objectMapper;

    public LuciConfigService(JdbcTemplate jdbcTemplate,
                             ResourcePatternResolver resourcePatternResolver,
                             @Lazy ObjectMapper objectMapper) {
        this.jdbcTemplate = jdbcTemplate;
        this.resourcePatternResolver = resourcePatternResolver;
        this.objectMapper = objectMapper;
    }

    public Map<String, Object> getMainConfig(String zone) {
        Map<String, String> values = new LinkedHashMap<>();
        List<Map<String, Object>> rows = jdbcTemplate.queryForList(
                "SELECT cfg_key, cfg_value FROM luci_main_config WHERE zone = ?",
                zone);
        for (Map<String, Object> row : rows) {
            Object key = row.get("CFG_KEY");
            Object value = row.get("CFG_VALUE");
            if (key != null) {
                values.put(String.valueOf(key), value == null ? null : String.valueOf(value));
            }
        }

        Map<String, Object> out = new LinkedHashMap<>();
        out.put("mqtt_enabled", parseBoolean(values.get("mqtt_enabled"), false));
        out.put("mqtt_url", valueOrEmpty(values.get("mqtt_url")));
        out.put("mqtt_username", valueOrEmpty(values.get("mqtt_username")));
        out.put("mqtt_password", "******");
        out.put("client_id", valueOrDefault(values.get("client_id"), "wakaama-lwm2mserver"));
        out.put("cloud_mqtt_like_enabled", parseBoolean(values.get("cloud_mqtt_like_enabled"), false));
        out.put("cloud_mqtt_like_target_peer_id", valueOrEmpty(values.get("cloud_mqtt_like_target_peer_id")));
        out.put("rest_api_enabled", parseBoolean(values.get("rest_api_enabled"), true));
        out.put("rest_api_port", parseInt(values.get("rest_api_port"), 8088));
        out.put("ha_reverse_proxy_enabled", parseBoolean(values.get("ha_reverse_proxy_enabled"), false));
        out.put("ha_reverse_proxy_host", valueOrDefault(values.get("ha_reverse_proxy_host"), "homeassistant.local"));
        out.put("ha_reverse_proxy_port", parseInt(values.get("ha_reverse_proxy_port"), 8123));
        return out;
    }

    public void upsertMainConfig(String zone, Map<String, Object> incoming) {
        upsertKey(zone, "mqtt_enabled", toFlag(incoming.get("mqtt_enabled")));
        upsertKey(zone, "mqtt_url", asString(incoming.get("mqtt_url")));
        upsertKey(zone, "mqtt_username", asString(incoming.get("mqtt_username")));

        String password = asString(incoming.get("mqtt_password"));
        if (!password.isBlank() && !"******".equals(password)) {
            upsertKey(zone, "mqtt_password", password);
        }

        upsertKey(zone, "client_id", asString(incoming.get("client_id")));
        upsertKey(zone, "cloud_mqtt_like_enabled", toFlag(incoming.get("cloud_mqtt_like_enabled")));
        upsertKey(zone, "cloud_mqtt_like_target_peer_id", asString(incoming.get("cloud_mqtt_like_target_peer_id")));
        upsertKey(zone, "rest_api_enabled", toFlag(incoming.get("rest_api_enabled")));
        upsertKey(zone, "rest_api_port", asString(incoming.get("rest_api_port")));
        upsertKey(zone, "ha_reverse_proxy_enabled", toFlag(incoming.get("ha_reverse_proxy_enabled")));
        upsertKey(zone, "ha_reverse_proxy_host", asString(incoming.get("ha_reverse_proxy_host")));
        upsertKey(zone, "ha_reverse_proxy_port", asString(incoming.get("ha_reverse_proxy_port")));
    }

    public List<Map<String, Object>> listDevices(String zone) {
        return jdbcTemplate.query(
                "SELECT device_id, endpoint, serial, name, sampling_enabled, sampling_rate, report_rate, sleep_mode, "
                        + "sensor_list_uart_i2c, sensor_list_rs485, sampling_config_version "
                        + "FROM luci_devices WHERE zone = ? ORDER BY device_id",
                (rs, rowNum) -> {
                    Map<String, Object> row = new LinkedHashMap<>();
                    row.put("id", rs.getString("device_id"));
                    row.put("endpoint", valueOrEmpty(rs.getString("endpoint")));
                    row.put("serial", valueOrEmpty(rs.getString("serial")));
                    row.put("name", valueOrEmpty(rs.getString("name")));
                    row.put("sampling_enabled", rs.getBoolean("sampling_enabled"));
                    row.put("sampling_rate", rs.getInt("sampling_rate"));
                    row.put("report_rate", rs.getInt("report_rate"));
                    row.put("sleep_mode", valueOrDefault(rs.getString("sleep_mode"), "no"));
                    row.put("sensor_list_uart_i2c", valueOrEmpty(rs.getString("sensor_list_uart_i2c")));
                    row.put("sensor_list_rs485", valueOrEmpty(rs.getString("sensor_list_rs485")));
                    row.put("sampling_config_version", rs.getInt("sampling_config_version"));
                    return row;
                },
                zone);
    }

    public Map<String, Object> getDevice(String zone, String ref) {
        List<Map<String, Object>> rows = jdbcTemplate.query(
                "SELECT device_id, endpoint, serial, name, sampling_enabled, sampling_rate, report_rate, sleep_mode, "
                        + "sensor_list_uart_i2c, sensor_list_rs485, sampling_config_version "
                        + "FROM luci_devices WHERE zone = ? AND (device_id = ? OR endpoint = ?)",
                (rs, rowNum) -> {
                    Map<String, Object> row = new LinkedHashMap<>();
                    row.put("id", rs.getString("device_id"));
                    row.put("endpoint", valueOrEmpty(rs.getString("endpoint")));
                    row.put("serial", valueOrEmpty(rs.getString("serial")));
                    row.put("name", valueOrEmpty(rs.getString("name")));
                    row.put("sampling_enabled", rs.getBoolean("sampling_enabled"));
                    row.put("sampling_rate", rs.getInt("sampling_rate"));
                    row.put("report_rate", rs.getInt("report_rate"));
                    row.put("sleep_mode", valueOrDefault(rs.getString("sleep_mode"), "no"));
                    row.put("sensor_list_uart_i2c", valueOrEmpty(rs.getString("sensor_list_uart_i2c")));
                    row.put("sensor_list_rs485", valueOrEmpty(rs.getString("sensor_list_rs485")));
                    row.put("sampling_config_version", rs.getInt("sampling_config_version"));
                    return row;
                },
                zone, ref, ref);

        return rows.isEmpty() ? null : rows.get(0);
    }

    public int updateDevice(String zone, String ref, Map<String, Object> incoming) {
        List<String> ids = jdbcTemplate.query(
                "SELECT device_id FROM luci_devices WHERE zone = ? AND (device_id = ? OR endpoint = ?)",
                (rs, rowNum) -> rs.getString("device_id"),
                zone, ref, ref);

        if (ids.isEmpty()) {
            return 0;
        }

        String deviceId = ids.get(0);

        if (incoming.containsKey("name")) {
            upsertDeviceColumn(zone, deviceId, "name", asString(incoming.get("name")));
        }
        if (incoming.containsKey("sampling_enabled")) {
            upsertDeviceColumn(zone, deviceId, "sampling_enabled", parseBooleanObject(incoming.get("sampling_enabled")) ? "1" : "0");
        }
        if (incoming.containsKey("sampling_rate")) {
            upsertDeviceColumn(zone, deviceId, "sampling_rate", Integer.toString(parseIntObject(incoming.get("sampling_rate"), 60)));
        }
        if (incoming.containsKey("report_rate")) {
            upsertDeviceColumn(zone, deviceId, "report_rate", Integer.toString(parseIntObject(incoming.get("report_rate"), 300)));
        }
        if (incoming.containsKey("sleep_mode")) {
            upsertDeviceColumn(zone, deviceId, "sleep_mode", asString(incoming.get("sleep_mode")));
        }
        if (incoming.containsKey("sensor_list_uart_i2c")) {
            upsertDeviceColumn(zone, deviceId, "sensor_list_uart_i2c", asString(incoming.get("sensor_list_uart_i2c")));
        }
        if (incoming.containsKey("sensor_list_rs485")) {
            upsertDeviceColumn(zone, deviceId, "sensor_list_rs485", asString(incoming.get("sensor_list_rs485")));
        }

        jdbcTemplate.update(
                "UPDATE luci_devices SET sampling_config_version = sampling_config_version + 1, updated_at = CURRENT_TIMESTAMP "
                        + "WHERE zone = ? AND device_id = ?",
                zone,
                deviceId);

        return 1;
    }

    public List<Map<String, Object>> listSensors(String zone, String group) {
        List<Map<String, Object>> defaults = listDefaultSensors(group);
        List<Map<String, Object>> dbRows = jdbcTemplate.query(
                "SELECT sensor_id, sensor_version, display_name, driver_json FROM luci_sensors WHERE zone = ? AND sensor_group = ? ORDER BY sensor_id, sensor_version",
                (rs, rowNum) -> {
                    Map<String, Object> row = new LinkedHashMap<>();
                    String baseId = valueOrEmpty(rs.getString("sensor_id")).trim();
                    String version = normalizeVersion(rs.getString("sensor_version"));
                    String combinedId = buildCombinedSensorId(baseId, version);
                    row.put("id", combinedId);
                    row.put("base_id", baseId);
                    row.put("version", version);
                    String name = valueOrEmpty(rs.getString("display_name"));
                    row.put("name", name);
                    row.put("label", buildSensorLabel(name, combinedId, version));
                    row.put("driver_json", valueOrEmpty(rs.getString("driver_json")));
                    return row;
                },
                zone,
                group);

        LinkedHashMap<String, Map<String, Object>> mergedById = new LinkedHashMap<>();
        for (Map<String, Object> row : defaults) {
            mergedById.put(asString(row.get("id")), new LinkedHashMap<>(row));
        }

        for (Map<String, Object> row : dbRows) {
            String id = asString(row.get("id"));
            if (id.isBlank()) {
                continue;
            }
            Map<String, Object> existing = mergedById.get(id);
            if (existing == null) {
                mergedById.put(id, new LinkedHashMap<>(row));
                continue;
            }

            String dbName = asString(row.get("name"));
            String dbDriverJson = asString(row.get("driver_json"));
            String dbVersion = normalizeVersion(row.get("version"));
            existing.put("version", dbVersion);
            if (!dbName.isBlank()) {
                existing.put("name", dbName);
            }
            if (!dbDriverJson.isBlank()) {
                existing.put("driver_json", dbDriverJson);
            }

            String displayName = asString(existing.get("name"));
            existing.put("label", buildSensorLabel(displayName, id, dbVersion));
        }

        for (Map<String, Object> row : mergedById.values()) {
            String name = asString(row.get("name"));
            String version = normalizeVersion(row.get("version"));
            String id = asString(row.get("id"));
            row.put("version", version);
            row.put("label", buildSensorLabel(name, id, version));
        }

        return new ArrayList<>(mergedById.values());
    }

    private List<Map<String, Object>> listDefaultSensors(String group) {
        List<Map<String, Object>> items = new ArrayList<>();
        try {
            Resource[] resources = resourcePatternResolver.getResources("classpath*:sensors/" + group + "/*.json");
            List<Resource> sorted = new ArrayList<>();
            for (Resource resource : resources) {
                sorted.add(resource);
            }
            sorted.sort(Comparator.comparing(Resource::getFilename, Comparator.nullsLast(String::compareTo)));

            for (Resource resource : sorted) {
                try {
                    Map<String, Object> parsed = objectMapper.readValue(resource.getInputStream(), MAP_TYPE);
                    String sensorId = buildSensorId(parsed, resource.getFilename());
                    if (sensorId.isBlank()) {
                        continue;
                    }

                    SensorIdentity sensorIdentity = parseCombinedSensorId(sensorId);
                    String combinedId = buildCombinedSensorId(sensorIdentity.id(), sensorIdentity.version());

                    Map<String, Object> row = new LinkedHashMap<>();
                    row.put("id", combinedId);
                    row.put("base_id", sensorIdentity.id());
                    row.put("version", sensorIdentity.version());
                    String name = asString(parsed.get("name"));
                    row.put("name", name);
                    row.put("label", buildSensorLabel(name, combinedId, sensorIdentity.version()));
                    row.put("driver_json", objectMapper.writeValueAsString(parsed));
                    items.add(row);
                } catch (Exception resourceError) {
                    LOG.warn("Skipping unreadable default sensor resource {} for group {}", resource.getFilename(), group, resourceError);
                }
            }
        } catch (Exception e) {
            LOG.warn("Failed to read default sensor definitions for group {}", group, e);
        }

        return items;
    }

    private static String buildSensorId(Map<String, Object> parsed, String fallbackFileName) {
        String id = asString(parsed.get("id"));
        String version = asString(parsed.get("version"));

        if (!id.isBlank() && !version.isBlank()) {
            return id + "-" + version;
        }
        if (!id.isBlank()) {
            return id;
        }

        if (fallbackFileName == null || fallbackFileName.isBlank()) {
            return "";
        }
        int dot = fallbackFileName.lastIndexOf('.');
        return dot > 0 ? fallbackFileName.substring(0, dot) : fallbackFileName;
    }

        public void upsertSensor(String zone, String group, String sensorId, String sensorVersion, String displayName, String driverJson) {
        String normalizedVersion = normalizeVersion(sensorVersion);
        jdbcTemplate.update(
            "MERGE INTO luci_sensors (zone, sensor_group, sensor_id, sensor_version, display_name, driver_json, updated_at) "
                + "KEY(zone, sensor_group, sensor_id, sensor_version) VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)",
                zone,
                group,
                sensorId,
            normalizedVersion,
                displayName,
                driverJson);
    }

        public int deleteSensor(String zone, String group, String sensorId, String sensorVersion) {
        String normalizedVersion = normalizeVersion(sensorVersion);
        return jdbcTemplate.update(
            "DELETE FROM luci_sensors WHERE zone = ? AND sensor_group = ? AND sensor_id = ? AND sensor_version = ?",
                zone,
                group,
            sensorId,
            normalizedVersion);
    }

    public void ensureDeviceRecord(String zone, String endpoint, String serial) {
        if (zone == null || zone.isBlank()) {
            return;
        }

        String normalizedEndpoint = endpoint == null ? "" : endpoint.trim();
        String normalizedSerial = serial == null ? "" : serial.trim();
        String deviceId = !normalizedSerial.isBlank() ? normalizedSerial : normalizedEndpoint;
        if (deviceId.isBlank()) {
            return;
        }

        jdbcTemplate.update(
                "MERGE INTO luci_devices (zone, device_id, endpoint, serial, updated_at) "
                        + "KEY(zone, device_id) VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)",
                zone,
                deviceId,
                normalizedEndpoint.isBlank() ? null : normalizedEndpoint,
                normalizedSerial.isBlank() ? null : normalizedSerial);
    }

    public String nextSensorId(String zone, String group) {
        List<String> ids = jdbcTemplate.query(
                "SELECT sensor_id FROM luci_sensors WHERE zone = ? AND sensor_group = ?",
                (rs, rowNum) -> rs.getString("sensor_id"),
                zone,
                group);

        int max = 0;
        for (String id : ids) {
            String normalized = id == null ? "" : id.trim();
            if (normalized.isBlank()) {
                continue;
            }
            try {
                max = Math.max(max, Integer.parseInt(normalized));
            } catch (NumberFormatException ignored) {
                // Ignore malformed IDs.
            }
        }

        return Integer.toString(max + 1);
    }

    public SensorIdentity parseSensorIdentity(String combinedId, String explicitVersion) {
        String normalizedVersion = normalizeVersion(explicitVersion);
        if (combinedId == null || combinedId.isBlank()) {
            return new SensorIdentity("", normalizedVersion);
        }

        SensorIdentity parsed = parseCombinedSensorId(combinedId);
        if (!explicitVersion.isBlank()) {
            return new SensorIdentity(parsed.id(), normalizedVersion);
        }
        return parsed;
    }

    public String formatCombinedSensorId(String sensorId, String sensorVersion) {
        return buildCombinedSensorId(asString(sensorId), normalizeVersion(sensorVersion));
    }

    private static SensorIdentity parseCombinedSensorId(String value) {
        String normalized = asString(value);
        int idx = normalized.lastIndexOf('-');
        if (idx <= 0 || idx >= normalized.length() - 1) {
            return new SensorIdentity(normalized, "1");
        }

        String id = normalized.substring(0, idx).trim();
        String versionCandidate = normalized.substring(idx + 1).trim();
        if (id.isBlank()) {
            return new SensorIdentity(normalized, "1");
        }
        if (versionCandidate.isBlank()) {
            return new SensorIdentity(id, "1");
        }
        return new SensorIdentity(id, normalizeVersion(versionCandidate));
    }

    private static String normalizeVersion(Object value) {
        String normalized = asString(value);
        return normalized.isBlank() ? "1" : normalized;
    }

    private static String buildCombinedSensorId(String id, String version) {
        String normalizedId = asString(id);
        String normalizedVersion = normalizeVersion(version);
        if (normalizedId.isBlank()) {
            return "";
        }
        return normalizedId + "-" + normalizedVersion;
    }

    private static String buildSensorLabel(String name, String combinedId, String version) {
        String base = asString(name);
        if (base.isBlank()) {
            base = asString(combinedId);
        }
        return base + " {v" + normalizeVersion(version) + "}";
    }

    public record SensorIdentity(String id, String version) {
    }

    private void upsertKey(String zone, String key, String value) {
        if (value == null || value.isBlank()) {
            return;
        }

        jdbcTemplate.update(
                "MERGE INTO luci_main_config (zone, cfg_key, cfg_value, updated_at) KEY(zone, cfg_key) VALUES (?, ?, ?, CURRENT_TIMESTAMP)",
                zone,
                key,
                value);
    }

    private void upsertDeviceColumn(String zone, String deviceId, String column, String value) {
        if (value == null) {
            return;
        }

        jdbcTemplate.update(
                "UPDATE luci_devices SET " + column + " = ?, updated_at = CURRENT_TIMESTAMP WHERE zone = ? AND device_id = ?",
                value,
                zone,
                deviceId);
    }

    private static String valueOrEmpty(String value) {
        return value == null ? "" : value;
    }

    private static String valueOrDefault(String value, String fallback) {
        return (value == null || value.isBlank()) ? fallback : value;
    }

    private static String asString(Object value) {
        return value == null ? "" : String.valueOf(value).trim();
    }

    private static boolean parseBoolean(String value, boolean fallback) {
        if (value == null || value.isBlank()) {
            return fallback;
        }
        return "1".equals(value) || "true".equalsIgnoreCase(value);
    }

    private static boolean parseBooleanObject(Object value) {
        if (value == null) {
            return false;
        }
        if (value instanceof Boolean b) {
            return b;
        }
        String s = String.valueOf(value).trim();
        return "1".equals(s) || "true".equalsIgnoreCase(s);
    }

    private static int parseInt(String value, int fallback) {
        if (value == null || value.isBlank()) {
            return fallback;
        }
        try {
            return Integer.parseInt(value.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static int parseIntObject(Object value, int fallback) {
        if (value == null) {
            return fallback;
        }
        try {
            return Integer.parseInt(String.valueOf(value).trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static String toFlag(Object value) {
        return parseBooleanObject(value) ? "1" : "0";
    }
}
