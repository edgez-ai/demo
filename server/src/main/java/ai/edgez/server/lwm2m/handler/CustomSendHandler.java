package ai.edgez.server.lwm2m.handler;

import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;
import java.util.Map;

import com.fasterxml.jackson.core.type.TypeReference;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.eclipse.leshan.core.node.LwM2mNode;
import org.eclipse.leshan.core.node.LwM2mResource;
import org.eclipse.leshan.core.node.LwM2mResourceInstance;
import org.eclipse.leshan.core.request.ContentFormat;
import org.eclipse.leshan.core.node.TimestampedLwM2mNodes;
import org.eclipse.leshan.core.request.WriteRequest;
import org.eclipse.leshan.core.request.SendRequest;
import org.eclipse.leshan.core.response.WriteResponse;
import org.eclipse.leshan.server.LeshanServer;
import org.eclipse.leshan.server.registration.Registration;
import org.eclipse.leshan.server.send.SendListener;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import ai.edgez.server.lwm2m.util.LuciConfigService;
import ai.edgez.server.lwm2m.util.SecretKeyService;
import ai.edgez.server.lwm2m.util.SampleReportMongoService;

/**
 * Basic Send listener used to log inbound Send requests. The server reference is
 * injected so it can be extended later if forwarding or additional processing
 * is needed.
 */
public class CustomSendHandler implements SendListener {
    private static final Logger LOG = LoggerFactory.getLogger(CustomSendHandler.class);
    private static final ObjectMapper OBJECT_MAPPER = new ObjectMapper();
    private static final int SAMPLE_OBJECT_ID = 31024;
    private static final int SAMPLE_VERSION_RESOURCE_INSTANCE = 0;
    private static final int SAMPLE_VERSION_RESOURCE_ID = 0;
    private static final int SAMPLE_CONFIG_INSTANCE_ID = 1;
    private static final int SAMPLE_CONFIG_VERSION_RESOURCE_ID = 0;
    private static final long SAMPLE_CONFIG_SYNC_TIMEOUT_MS = 5000L;

    private LeshanServer leshanServer;
    private SampleReportMongoService sampleReportMongoService;
    private SecretKeyService secretKeyService;
    private LuciConfigService luciConfigService;

    public void setLeshanServer(LeshanServer leshanServer) {
        this.leshanServer = leshanServer;
    }

    public void setSampleReportMongoService(SampleReportMongoService sampleReportMongoService) {
        this.sampleReportMongoService = sampleReportMongoService;
    }

    public void setSecretKeyService(SecretKeyService secretKeyService) {
        this.secretKeyService = secretKeyService;
    }

    public void setLuciConfigService(LuciConfigService luciConfigService) {
        this.luciConfigService = luciConfigService;
    }

    @Override
    public void dataReceived(Registration registration, TimestampedLwM2mNodes data, SendRequest request) {
        String endpoint = registration != null ? registration.getEndpoint() : "unknown";

        if (data == null || data.getMostRecentNodes() == null) {
            LOG.info("Sample report received from [{}] but payload is empty", endpoint);
            return;
        }

        String report = String.valueOf(data.getMostRecentNodes());
        LOG.info("Sample report received from [{}]: {}", endpoint, report);

        if (sampleReportMongoService == null) {
            LOG.info("Mongo report writer disabled/unavailable for endpoint=[{}]", endpoint);
        }

        if (sampleReportMongoService != null) {
            try {
                persistAsRelayStyleRecords(registration, data);
            } catch (RuntimeException e) {
                LOG.warn("Failed to save sample report to MongoDB for [{}]: {}", endpoint, e.getMessage());
            }
        }

        // Keep debug output for full raw object inspection when needed.
        if (LOG.isDebugEnabled()) {
            LOG.debug("Send data received from [{}]: {}", endpoint, data);
        }
    }

    @Override
    public void onError(Registration registration, String errorMessage, Exception error) {
        LOG.warn("Error while handling Send request from [{}]: {}", registration != null ? registration.getEndpoint() : "unknown", errorMessage, error);
    }

    private void persistAsRelayStyleRecords(Registration registration, TimestampedLwM2mNodes data) {
        if (sampleReportMongoService == null || data == null || data.getMostRecentNodes() == null) {
            return;
        }

        String peerId = registration != null && registration.getId() != null ? registration.getId() : "";
        String serial = registration != null && registration.getEndpoint() != null ? registration.getEndpoint() : "";
        String zone = deriveZoneFromEndpoint(serial);
        Instant timestamp = Instant.now();

        Map<?, ?> nodes = data.getMostRecentNodes();
        for (Map.Entry<?, ?> entry : nodes.entrySet()) {
            LOG.info("Sample report node key received: endpoint=[{}] key=[{}]", serial, entry.getKey());

            ParsedPath path = parsePath(entry.getKey());
            if (path == null) {
                LOG.info("Skipping sample report node: endpoint=[{}] key=[{}] reason=unparseable-path", serial, entry.getKey());
                continue;
            }

            if (path.object == SAMPLE_OBJECT_ID
                    && path.instance == SAMPLE_VERSION_RESOURCE_INSTANCE
                    && path.resource == SAMPLE_VERSION_RESOURCE_ID) {
                Integer reportedVersion = extractNumericVersion(entry.getValue());
                if (reportedVersion != null) {
                    triggerSampleConfigUpdateIfMismatch(registration, serial, reportedVersion.intValue());
                }
            }

            List<RelayRecord> records = buildRelayRecords(path, entry.getValue());
            for (RelayRecord record : records) {
                if (record.object == 31024) {
                    LOG.debug("Skipping sample report record: endpoint=[{}] path={}/{}/{} reason=object-31024",
                            serial,
                            record.object,
                            record.instance,
                            record.resource);
                    continue;
                }

                LOG.info("Persisting sample report node: endpoint=[{}] path={}/{}/{} type={} value={}",
                        serial,
                        record.object,
                        record.instance,
                        record.resource,
                        record.type,
                        record.value);

                sampleReportMongoService.saveResourceReport(
                        peerId,
                        serial,
                        zone,
                        record.object,
                        record.instance,
                        record.resource,
                        record.type,
                        record.value,
                        timestamp);
            }
        }
    }

    private Integer extractNumericVersion(Object nodeObj) {
        if (nodeObj instanceof LwM2mResource resourceNode) {
            Object value = resourceNode.getValue();
            if (value instanceof Number number) {
                return number.intValue();
            }
            if (value instanceof String text) {
                try {
                    return Integer.parseInt(text.trim());
                } catch (NumberFormatException ignored) {
                    return null;
                }
            }
        }
        return null;
    }

    private void triggerSampleConfigUpdateIfMismatch(Registration registration, String endpoint, int reportedVersion) {
        if (registration == null || endpoint == null || endpoint.isBlank()) {
            return;
        }
        if (luciConfigService == null || secretKeyService == null || leshanServer == null) {
            return;
        }

        String zone = secretKeyService.findManagedZone(endpoint);
        if (zone == null || zone.isBlank()) {
            return;
        }

        Map<String, Object> device = luciConfigService.getDevice(zone, endpoint);
        if (device == null || device.isEmpty()) {
            return;
        }

        Integer latestVersion = parsePositiveInt(device.get("sampling_config_version"));
        if (latestVersion == null) {
            return;
        }

        if (latestVersion.intValue() == reportedVersion) {
            return;
        }

        LOG.info("Sample config version mismatch endpoint=[{}] zone={} reported={} latest={} -> triggering /{}/{}/{} update",
                endpoint,
                zone,
                reportedVersion,
                latestVersion,
                SAMPLE_OBJECT_ID,
                SAMPLE_CONFIG_INSTANCE_ID,
                SAMPLE_CONFIG_VERSION_RESOURCE_ID);

        try {
            WriteRequest versionWrite = new WriteRequest(
                    ContentFormat.TEXT,
                    SAMPLE_OBJECT_ID,
                    SAMPLE_CONFIG_INSTANCE_ID,
                    SAMPLE_CONFIG_VERSION_RESOURCE_ID,
                    latestVersion.longValue());
            WriteResponse response = leshanServer.send(registration, versionWrite, SAMPLE_CONFIG_SYNC_TIMEOUT_MS);
            if (response == null || !response.isSuccess()) {
                LOG.warn("Sample config version sync failed endpoint=[{}] reported={} latest={} code={}",
                        endpoint,
                        reportedVersion,
                        latestVersion,
                        response == null ? "(null)" : response.getCode());
            } else {
                LOG.info("Sample config version sync succeeded endpoint=[{}] -> latest={} on /{}/{}/{}",
                        endpoint,
                        latestVersion,
                        SAMPLE_OBJECT_ID,
                        SAMPLE_CONFIG_INSTANCE_ID,
                        SAMPLE_CONFIG_VERSION_RESOURCE_ID);
            }
        } catch (Exception e) {
            LOG.warn("Sample config version sync error endpoint=[{}] reported={} latest={}",
                    endpoint,
                    reportedVersion,
                    latestVersion,
                    e);
        }
    }

    private Integer parsePositiveInt(Object value) {
        if (value == null) {
            return null;
        }
        try {
            int parsed = Integer.parseInt(String.valueOf(value).trim());
            return parsed > 0 ? parsed : null;
        } catch (NumberFormatException ignored) {
            return null;
        }
    }

    private List<RelayRecord> buildRelayRecords(ParsedPath path, Object nodeObj) {
        List<RelayRecord> records = new ArrayList<>();
        if (nodeObj == null) {
            records.add(new RelayRecord(path.object, path.instance, path.resource, "null", null));
            return records;
        }

        if (nodeObj instanceof LwM2mResource resourceNode) {
            Object value = resourceNode.getValue();
            String type = normalizeType(resourceNode.getType() != null ? resourceNode.getType().name() : null, value);
            records.addAll(extractFromOpaqueOrFallback(path, type, value));
            return records;
        }

        if (nodeObj instanceof LwM2mResourceInstance resourceInstance) {
            Object value = resourceInstance.getValue();
            String type = normalizeType(resourceInstance.getType() != null ? resourceInstance.getType().name() : null, value);
            records.addAll(extractFromOpaqueOrFallback(path, type, value));
            return records;
        }

        if (nodeObj instanceof LwM2mNode) {
            records.add(new RelayRecord(path.object, path.instance, path.resource, "object", String.valueOf(nodeObj)));
            return records;
        }

        records.add(new RelayRecord(path.object, path.instance, path.resource, inferType(nodeObj), nodeObj));
        return records;
    }

    private List<RelayRecord> extractFromOpaqueOrFallback(ParsedPath path, String type, Object value) {
        List<RelayRecord> records = new ArrayList<>();
        if ("opaque".equals(type) && value instanceof byte[] rawBytes) {
            List<RelayRecord> decoded = decodeOpaqueRelayRecords(rawBytes);
            if (!decoded.isEmpty()) {
                return decoded;
            }
            records.add(new RelayRecord(path.object, path.instance, path.resource, type, Base64.getEncoder().encodeToString(rawBytes)));
            return records;
        }

        records.add(new RelayRecord(path.object, path.instance, path.resource, type, value));
        return records;
    }

    private List<RelayRecord> decodeOpaqueRelayRecords(byte[] rawBytes) {
        List<RelayRecord> records = new ArrayList<>();
        try {
            Map<String, Object> map = OBJECT_MAPPER.readValue(rawBytes, new TypeReference<Map<String, Object>>() {});
            Integer object = readInt(map, "o", "lwm2m_object", "object");
            Integer instance = readInt(map, "i", "instance");
            Integer resource = readInt(map, "r", "resource");
            Object value = readAny(map, "v", "value");
            if (object != null && instance != null && resource != null) {
                records.add(new RelayRecord(object, instance, resource, inferType(value), value));
                return records;
            }
        } catch (Exception ignored) {
            // fall back below
        }
        return records;
    }

    private Integer readInt(Map<String, Object> map, String... keys) {
        for (String key : keys) {
            Object value = map.get(key);
            if (value == null) {
                continue;
            }
            if (value instanceof Number number) {
                return number.intValue();
            }
            if (value instanceof String text) {
                try {
                    return Integer.parseInt(text.trim());
                } catch (NumberFormatException ignored) {
                    // try next key
                }
            }
        }
        return null;
    }

    private Object readAny(Map<String, Object> map, String... keys) {
        for (String key : keys) {
            if (map.containsKey(key)) {
                return map.get(key);
            }
        }
        return null;
    }

    private String normalizeType(String resourceType, Object value) {
        if (resourceType == null) {
            return inferType(value);
        }

        return switch (resourceType.trim().toUpperCase()) {
            case "INTEGER", "TIME" -> "int";
            case "FLOAT" -> "float";
            case "STRING", "OBJLNK" -> "string";
            case "BOOLEAN" -> "bool";
            case "OPAQUE" -> "opaque";
            default -> inferType(value);
        };
    }

    private String inferType(Object value) {
        if (value == null) {
            return "null";
        }
        if (value instanceof Boolean) {
            return "bool";
        }
        if (value instanceof Byte || value instanceof Short || value instanceof Integer || value instanceof Long) {
            return "int";
        }
        if (value instanceof Float || value instanceof Double) {
            return "float";
        }
        if (value instanceof Map) {
            return "object";
        }
        if (value instanceof List) {
            return "array";
        }
        if (value instanceof byte[]) {
            return "opaque";
        }
        return "string";
    }

    private ParsedPath parsePath(Object pathObj) {
        if (pathObj == null) {
            return null;
        }

        String raw = String.valueOf(pathObj).trim();
        if (raw.isEmpty()) {
            return null;
        }

        if (raw.startsWith("/")) {
            raw = raw.substring(1);
        }

        String[] parts = raw.split("/");
        if (parts.length < 3) {
            return null;
        }

        try {
            int object = Integer.parseInt(parts[0]);
            int instance = Integer.parseInt(parts[1]);
            int resource = Integer.parseInt(parts[2]);
            return new ParsedPath(object, instance, resource);
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private String deriveZoneFromEndpoint(String endpoint) {
        if (endpoint == null) {
            return "";
        }

        String trimmed = endpoint.trim();
        if (trimmed.isEmpty()) {
            return "";
        }

        int delimiter = trimmed.indexOf('-');
        if (delimiter <= 0) {
            return trimmed;
        }

        return trimmed.substring(0, delimiter);
    }

    private static final class ParsedPath {
        private final int object;
        private final int instance;
        private final int resource;

        private ParsedPath(int object, int instance, int resource) {
            this.object = object;
            this.instance = instance;
            this.resource = resource;
        }
    }

    private static final class RelayRecord {
        private final int object;
        private final int instance;
        private final int resource;
        private final String type;
        private final Object value;

        private RelayRecord(int object, int instance, int resource, String type, Object value) {
            this.object = object;
            this.instance = instance;
            this.resource = resource;
            this.type = type;
            this.value = value;
        }
    }
}
